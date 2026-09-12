#include "../Utils/pch.h"
#include "OCREngine.h"
#include "../Utils/Logger.h"
#include <winrt/Windows.Globalization.h>

// IMemoryBufferByteAccess – WinRT SoftwareBitmap piksel verisine erişim için gerekli.
// Windows SDK memorybuffer.h yerine burada tanımlamak daha taşınabilir.
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d"))
IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetBuffer(BYTE** value, UINT32* capacity) = 0;
};

static std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Metnin cümle sonu noktalamasıyla (. ! ? …) bittiğini kontrol eder.
// Sondaki kapanış tırnak/parantez/boşluklar atlanarak asıl karaktere bakılır.
static bool EndsWithSentenceTerminator(const std::wstring& text)
{
    size_t i = text.size();
    while (i > 0 && (text[i - 1] == L'"' || text[i - 1] == L'\'' ||
                      text[i - 1] == L')' || text[i - 1] == L']' || text[i - 1] == L' '))
        --i;
    if (i == 0) return false;
    wchar_t c = text[i - 1];
    return c == L'.' || c == L'!' || c == L'?' || c == L'\u2026';
}

// Satırın küçük harfle başlaması, önceki cümlenin DEVAMI olduğunun güçlü bir
// işaretidir (yeni bir başlık/cümle İngilizce'de genelde büyük harfle başlar).
// İlk alfabetik karaktere bakılır, rakam/noktalama gibi harf olmayanlar atlanır.
static bool StartsWithLowercase(const std::wstring& text)
{
    for (wchar_t c : text)
    {
        if (std::iswalpha(static_cast<wint_t>(c)))
            return std::iswlower(static_cast<wint_t>(c)) != 0;
    }
    return false; // harf yoksa devam sinyali sayma
}

// Bölgenin piksellerinden BASKIN YAZI RENGİNİ kabaca tahmin eder. Tam bir
// metin/arka-plan segmentasyonu değildir: kutunun ortalama parlaklığından
// belirgin şekilde sapan pikseller (yazı vuruşları olması muhtemel) ayrıca
// ortalanır. İki satırın "aynı renkte mi" olduğunu karşılaştırmak için
// yeterince tutarlı bir tahmin verir. Düşük kontrastta (yazı ayırt
// edilemiyorsa) false döner.
// Bölgenin piksellerinden hem baskın YAZI rengini (ortalama parlaklıktan
// belirgin sapan "aykırı" pikseller – muhtemelen yazı vuruşları) hem de
// baskın ARKA PLAN rengini (çoğunluk pikselleri) TEK geçişte tahmin eder.
// Tam bir metin/arka-plan segmentasyonu değildir, ama overlay kutusunu
// görüntüyle uyumlu (sinerjik) hale getirmek ve iki satırın "aynı renkte
// mi" olduğunu karşılaştırmak için yeterince tutarlı bir tahmin verir.
static bool EstimateRegionColors(const CaptureFrame& frame, const RECT& box,
                                  uint8_t& fgR, uint8_t& fgG, uint8_t& fgB, bool& hasFg,
                                  uint8_t& bgR, uint8_t& bgG, uint8_t& bgB, bool& hasBg)
{
    hasFg = hasBg = false;
    if (frame.bgra.empty()) return false;

    // En fazla ~48x48 nokta örnekle – kutu ne kadar büyük olursa olsun
    // (uzun, çok satırlı birleştirilmiş paragraflarda kutu epey büyüyebilir)
    // taranan piksel sayısı SABİT bir üst sınırda kalır. Renk ortalaması
    // için bu kadar örnek fazlasıyla yeterli; tam piksel taraması gereksiz
    // CPU maliyeti demekti.
    constexpr int kMaxSamplesPerAxis = 48;

    int bx0 = std::max(0, static_cast<int>(box.left));
    int by0 = std::max(0, static_cast<int>(box.top));
    int bx1 = std::min(frame.width,  static_cast<int>(box.right));
    int by1 = std::min(frame.height, static_cast<int>(box.bottom));

    // ── Arka plan rengi: GENİŞLETİLMİŞ (dolgulu) kutunun, SIKI kutuyu
    // ÇEVRELEYEN kısmından ÖLÇÜLÜR — sıkı kutunun İÇİ BİLEREK HARİÇ
    // TUTULUR. Böylece bu tahmin metin vuruşlarından TAMAMEN ARINMIŞ olur ve
    // aşağıdaki yazı rengi tespitinde GÜVENİLİR bir referans olarak
    // kullanılabilir (bkz. aşağıdaki ön plan bloğundaki yorum – bu, önceki
    // "kutu içi ortalamadan sapma" yönteminin tersine dönmüş renk tespiti
    // hatasını çözmenin anahtarıdır).
    int h    = by1 - by0;
    int padX = std::max(4, static_cast<int>(h * 0.6));
    int padY = std::max(4, static_cast<int>(h * 0.6));
    int px0 = std::max(0, bx0 - padX);
    int py0 = std::max(0, by0 - padY);
    int px1 = std::min(frame.width,  bx1 + padX);
    int py1 = std::min(frame.height, by1 + padY);

    if (px1 > px0 && py1 > py0)
    {
        int stepX = std::max(1, (px1 - px0) / kMaxSamplesPerAxis);
        int stepY = std::max(1, (py1 - py0) / kMaxSamplesPerAxis);

        // HATA DÜZELTMESİ (kullanıcı bildirimi: "arka plan rengini
        // tutturamıyor, bariz beyaz olsa bile genelde gri yapıyor"):
        // ÖNCEDEN burada BASİT ORTALAMA (mean) alınıyordu. Sorun: dolgulu
        // örnekleme halkası, sıkı OCR kutusunun HEMEN dışındaki pikselleri
        // de kapsıyor – ki bunların bir kısmı harflerin kenarındaki
        // ANTI-ALIASING/BULANIKLIK piksellerdir (örn. siyah yazı + beyaz
        // zemin kenarında oluşan GRİ bir geçiş şeridi, ya da fotoğraflanmış/
        // sıkıştırılmış bir ekranda görülen moiré/renk sızması). Bu azınlık
        // ama sistematik "kirli" pikseller, ortalamayı GERÇEK (baskın) arka
        // plan renginden UZAKLAŞTIRIP griye doğru çekiyordu – zemin bariz
        // beyaz olsa bile.
        //
        // Çözüm: ortalama yerine kaba bir HİSTOGRAM üzerinden EN SIK
        // GÖRÜLEN (mod) rengi buluyoruz. Gerçek arka plan alanı her zaman
        // örnekleme halkasının BÜYÜK ÇOĞUNLUĞUNU oluşturur (kenar-karışım
        // pikselleri azınlıktadır), bu yüzden mod tabanlı yaklaşım kenar
        // gürültüsünden ETKİLENMEZ. Her kanalı 16 seviyeye (4 bit)
        // yuvarlayıp kovalıyoruz (4096 olası kova) – renk örnekleme sayımız
        // zaten küçük (en fazla 48x48=2304) olduğundan bu son derece ucuz.
        struct Bucket { uint64_t r{}, g{}, b{}, count{}; };
        std::unordered_map<uint32_t, Bucket> hist;
        hist.reserve(64);

        for (int y = py0; y < py1; y += stepY)
        {
            bool insideTightY = (y >= by0 && y < by1);
            const uint8_t* row = frame.bgra.data() + (static_cast<size_t>(y) * frame.width + px0) * 4;
            for (int x = px0; x < px1; x += stepX, row += static_cast<size_t>(stepX) * 4)
            {
                if (insideTightY && x >= bx0 && x < bx1)
                    continue; // sıkı kutunun İÇİNİ ATLA (yazı vuruşlarıyla kirlenmesin)
                uint8_t b8 = row[0], g8 = row[1], r8 = row[2];
                uint32_t key = (static_cast<uint32_t>(r8 >> 4) << 8) |
                               (static_cast<uint32_t>(g8 >> 4) << 4) |
                                static_cast<uint32_t>(b8 >> 4);
                Bucket& bucket = hist[key];
                bucket.r += r8; bucket.g += g8; bucket.b += b8; ++bucket.count;
            }
        }

        const Bucket* best = nullptr;
        for (auto& [key, bucket] : hist)
            if (!best || bucket.count > best->count) best = &bucket;

        if (best && best->count > 0)
        {
            bgR = static_cast<uint8_t>(best->r / best->count);
            bgG = static_cast<uint8_t>(best->g / best->count);
            bgB = static_cast<uint8_t>(best->b / best->count);
            hasBg = true;
        }
    }

    // ── Ön plan (yazı) rengi: DAR/sıkı OCR kutusu üzerinden, YUKARIDA
    // GÜVENİLİR ŞEKİLDE hesaplanan arka plan rengine GÖRE ─────────────────
    // ÖNCEKİ (HATALI) yöntem, sıkı kutu İÇİNDEKİ pikselleri SADECE o
    // kutunun KENDİ ortalamasından ne kadar SAPTIĞINA bakarak "aykırı =
    // yazı" varsayıyordu. Bu, yazının kutuyu YOĞUN kapladığı durumlarda
    // (kalın/büyük punto font, ya da harfler arası boşluğu az, tamamı büyük
    // harfli kısa kelimeler gibi SIKI kırpılmış kutularda SIK rastlanan bir
    // durum) BAŞARISIZ oluyordu: yazı pikselleri kutunun ÇOĞUNLUĞUNU
    // oluşturduğunda, bu kez AZINLIKTA kalan ARKA PLAN pikselleri "aykırı"
    // sayılıp YANLIŞLIKLA yazı rengi olarak seçiliyordu. Bu, "orijinalde
    // beyaz yazı olduğu hâlde bazen siyah yazı + kalın beyaz anahat"
    // şeklinde bildirilen TERSİNE DÖNMÜŞ renk tespiti hatasının TAM OLARAK
    // kaynağıydı.
    //
    // Düzeltme: artık her pikseli, kutunun KENDİ ortalaması yerine YUKARIDA
    // BAĞIMSIZ ölçülen GERÇEK arka plan rengine olan renk mesafesine göre
    // sınıflandırıyoruz – arka plana YAKIN olanlar arka plan/boşluk, UZAK
    // olanlar yazı vuruşu sayılır. Bu, yazı kutuyu ne kadar yoğun kaplarsa
    // kaplasın (azınlık ya da çoğunluk fark etmeksizin) doğru çalışır.
    // Güvenilir bir arka plan referansı YOKSA (nadiren – kutu ekranın
    // kenarında ve dolgu alanı hiç örnek veremediyse) eski (kutu-içi
    // ortalamadan sapma) yönteme düşülür.
    if (bx1 > bx0 && by1 > by0)
    {
        int stepX = std::max(1, (bx1 - bx0) / kMaxSamplesPerAxis);
        int stepY = std::max(1, (by1 - by0) / kMaxSamplesPerAxis);

        double avgLum = 0.0;
        if (!hasBg)
        {
            double sumLum = 0.0; int n = 0;
            for (int y = by0; y < by1; y += stepY)
            {
                const uint8_t* row = frame.bgra.data() + (static_cast<size_t>(y) * frame.width + bx0) * 4;
                for (int x = bx0; x < bx1; x += stepX, row += static_cast<size_t>(stepX) * 4)
                { sumLum += 0.114 * row[0] + 0.587 * row[1] + 0.299 * row[2]; ++n; }
            }
            if (n > 0) avgLum = sumLum / n;
        }

        uint64_t sB = 0, sG = 0, sR = 0, cnt = 0;
        for (int y = by0; y < by1; y += stepY)
        {
            const uint8_t* row = frame.bgra.data() + (static_cast<size_t>(y) * frame.width + bx0) * 4;
            for (int x = bx0; x < bx1; x += stepX, row += static_cast<size_t>(stepX) * 4)
            {
                bool isText;
                if (hasBg)
                {
                    double dr = static_cast<double>(row[2]) - bgR;
                    double dg = static_cast<double>(row[1]) - bgG;
                    double db = static_cast<double>(row[0]) - bgB;
                    isText = (dr * dr + dg * dg + db * db) > (40.0 * 40.0 * 3.0); // arka plandan renk mesafesi eşiği
                }
                else
                {
                    double lum = 0.114 * row[0] + 0.587 * row[1] + 0.299 * row[2];
                    isText = std::abs(lum - avgLum) > 40.0; // yedek: kutu-içi ortalamadan sapma
                }
                if (isText) { sB += row[0]; sG += row[1]; sR += row[2]; ++cnt; }
            }
        }
        if (cnt > 0)
        {
            fgR = static_cast<uint8_t>(sR / cnt);
            fgG = static_cast<uint8_t>(sG / cnt);
            fgB = static_cast<uint8_t>(sB / cnt);
            hasFg = true;
        }
    }

    return hasFg || hasBg;
}

// İki tahmini rengin "belirgin şekilde farklı" olup olmadığını kontrol eder
// (Öklid mesafesi, kanal başına ~kabaca 60/255 birim eşik).
static bool ColorsSignificantlyDifferent(uint8_t r1, uint8_t g1, uint8_t b1,
                                          uint8_t r2, uint8_t g2, uint8_t b2)
{
    int dr = static_cast<int>(r1) - r2;
    int dg = static_cast<int>(g1) - g2;
    int db = static_cast<int>(b1) - b2;
    return (dr * dr + dg * dg + db * db) > 3 * 60 * 60;
}

// İki ham satır arasında kalan, DÜZ (metin içermeyen) bir dikdörtgen
// bölgenin ortalama rengini örnekler. EstimateRegionColors'tan farklı
// olarak burada "metin vuruşlarını dışla" mantığı YOK – amaç zaten metinsiz
// bir aralığın (satırlar arası boşluk/farklı panel sınırı) TEK, genel
// rengini bulmak. Satırların FARKLI KAPSAYICILARA (konteynerlara) ait olup
// olmadığını anlamak için kullanılır (bkz. çağıran taraftaki yorum).
static bool SampleStripAverageColor(const CaptureFrame& frame, const RECT& rect,
                                     uint8_t& r, uint8_t& g, uint8_t& b)
{
    constexpr int kMaxSamplesPerAxis = 24;
    int x0 = std::max(0, static_cast<int>(rect.left));
    int y0 = std::max(0, static_cast<int>(rect.top));
    int x1 = std::min(frame.width,  static_cast<int>(rect.right));
    int y1 = std::min(frame.height, static_cast<int>(rect.bottom));
    if (x1 <= x0 || y1 <= y0 || frame.bgra.empty()) return false;

    int stepX = std::max(1, (x1 - x0) / kMaxSamplesPerAxis);
    int stepY = std::max(1, (y1 - y0) / kMaxSamplesPerAxis);
    uint64_t sB = 0, sG = 0, sR = 0, cnt = 0;
    for (int y = y0; y < y1; y += stepY)
    {
        const uint8_t* row = frame.bgra.data() + (static_cast<size_t>(y) * frame.width + x0) * 4;
        for (int x = x0; x < x1; x += stepX, row += static_cast<size_t>(stepX) * 4)
        { sB += row[0]; sG += row[1]; sR += row[2]; ++cnt; }
    }
    if (cnt == 0) return false;
    r = static_cast<uint8_t>(sR / cnt);
    g = static_cast<uint8_t>(sG / cnt);
    b = static_cast<uint8_t>(sB / cnt);
    return true;
}

OCREngine::OCREngine(const OcrConfig& cfg, const std::wstring& srcLang)
    : m_cfg(cfg), m_srcLang(srcLang) {}

OCREngine::~OCREngine()
{
    m_running.store(false);
    m_queueCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

HRESULT OCREngine::Init()
{
    winrt::Windows::Media::Ocr::OcrEngine engine{ nullptr };

    if (m_srcLang.empty())
    {
        // Otomatik Algıla: kullanıcı profilinde kurulu "Yazı Tanıma" dil
        // paketlerinden WinRT'nin kendi mekanizmasıyla uygun olanı seçilir.
        engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine)
        {
            Logger::Error("OCR: Otomatik dil algılama başarısız – kurulu bir 'Yazı Tanıma' dil paketi "
                          "bulunamadı. Ayarlar > Zaman ve Dil > Dil > Dil ekle.");
            return E_FAIL;
        }
        Logger::Info("OCREngine: hazır (kaynak dil: otomatik algılama).");
    }
    else
    {
        if (!CheckLanguagePack(m_srcLang))
        {
            Logger::WarningF("OCR: {} dil paketi eksik. Ayarlar > Zaman ve Dil > Dil > Dil ekle "
                              "({}, Yazı tanıma).", WideToUtf8(m_srcLang), WideToUtf8(m_srcLang));
            return E_FAIL;
        }
        winrt::Windows::Globalization::Language lang(m_srcLang);
        engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(lang);
        if (!engine)
        {
            Logger::ErrorF("OCR: {} için OcrEngine oluşturulamadı.", WideToUtf8(m_srcLang));
            return E_FAIL;
        }
        Logger::InfoF("OCREngine: hazır (kaynak dil: {}).", WideToUtf8(m_srcLang));
    }

    m_ocrEngine = engine;
    m_running.store(true);
    m_thread = std::thread(&OCREngine::WorkerLoop, this);
    return S_OK;
}

HRESULT OCREngine::UpdateSourceLanguage(const std::wstring& newSrcLang)
{
    if (newSrcLang == m_srcLang) return S_OK; // değişiklik yok

    winrt::Windows::Media::Ocr::OcrEngine newEngine{ nullptr };
    if (newSrcLang.empty())
    {
        newEngine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!newEngine)
        {
            Logger::Error("OCR: Otomatik dil algılama için uygun dil paketi bulunamadı, mevcut dil korunuyor.");
            return E_FAIL;
        }
    }
    else
    {
        if (!CheckLanguagePack(newSrcLang))
        {
            Logger::WarningF("OCR: {} dil paketi eksik, mevcut dil korunuyor.", WideToUtf8(newSrcLang));
            return E_FAIL;
        }
        winrt::Windows::Globalization::Language lang(newSrcLang);
        newEngine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(lang);
        if (!newEngine)
        {
            Logger::ErrorF("OCR: {} için OcrEngine oluşturulamadı, mevcut dil korunuyor.", WideToUtf8(newSrcLang));
            return E_FAIL;
        }
    }

    {
        std::lock_guard lock(m_engineMutex);
        m_ocrEngine = newEngine;
        m_srcLang   = newSrcLang;
    }
    Logger::InfoF("OCREngine: kaynak dil canlı olarak güncellendi ({}).",
                  newSrcLang.empty() ? std::string("otomatik algılama") : WideToUtf8(newSrcLang));
    return S_OK;
}

bool OCREngine::CheckLanguagePack(const std::wstring& langTag)
{
    winrt::Windows::Globalization::Language lang(langTag);
    auto langs = winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
    for (auto l : langs)                       // WinRT projected type: copy cheap
        if (l.LanguageTag() == langTag) return true;
    return false;
}

void OCREngine::OpenLanguageSettingsPage()
{
    // `ms-settings:regionlanguage` HER Windows 10/11 sürümünde çalışan,
    // belgelenmiş bir URI şemasıdır - ekstra bir WinRT türüne/SDK sürümüne
    // bağımlı DEĞİLDİR (bkz. OCREngine.h'deki revizyon notu). Kullanıcıyı
    // doğrudan "Dil ve Bölge" sayfasına götürür; oradan ilgili dili seçip
    // "Seçenekler > Yazı Tanıma'yı ekle"ye tıklaması yeterlidir.
    HINSTANCE result = ::ShellExecuteW(nullptr, L"open", L"ms-settings:regionlanguage",
                                        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        Logger::WarningF("OCR: Ayarlar sayfası açılamadı (ShellExecute hata kodu: {}).",
                         reinterpret_cast<INT_PTR>(result));
    else
        Logger::Info("OCR: Windows Dil ve Bölge ayar sayfası açıldı.");
}

void OCREngine::SubmitFrame(CaptureFrame frame, OcrCallback cb)
{
    {
        std::lock_guard lock(m_queueMutex);
        // Kuyruk sığ tutulur: 2'den fazla item varsa eskiyi at
        while (m_queue.size() >= 2)
            m_queue.pop();
        m_queue.push({ std::move(frame), std::move(cb) });
    }
    m_queueCv.notify_one();
}

void OCREngine::WorkerLoop()
{
    while (true)
    {
        WorkItem item;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [&]{ return !m_queue.empty() || !m_running.load(); });
            if (!m_running.load() && m_queue.empty()) break;
            item = std::move(m_queue.front());
            m_queue.pop();
        }

        try
        {
            auto regions = RunOcr(item.frame);
            item.cb(std::move(regions));
        }
        catch (const std::exception& ex)
        {
            Logger::ErrorF("OCREngine worker exception: {}", ex.what());
        }
        catch (const winrt::hresult_error& ex)
        {
            Logger::ErrorF("OCREngine worker WinRT hatası: {:08X}",
                           static_cast<uint32_t>(ex.code()));
        }
    }
}

std::vector<OcrRegion> OCREngine::RunOcr(const CaptureFrame& frame)
{
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Media::Ocr;

    if (frame.bgra.empty()) return {};

    // ── SoftwareBitmap oluştur ───────────────────────────────────────────────
    SoftwareBitmap bitmap(
        BitmapPixelFormat::Bgra8,
        frame.width, frame.height,
        BitmapAlphaMode::Premultiplied);

    // ── Piksel verisini yaz (IMemoryBufferByteAccess ile) ───────────────────
    {
        auto buffer    = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
        auto reference = buffer.CreateReference();

        // reference.data() WinRT'de YOKTUR – IMemoryBufferByteAccess gerekir
        auto byteAccess = reference.as<IMemoryBufferByteAccess>();
        BYTE*   pixels   = nullptr;
        UINT32  capacity = 0;
        winrt::check_hresult(byteAccess->GetBuffer(&pixels, &capacity));

        std::memcpy(pixels,
                    frame.bgra.data(),
                    std::min<size_t>(capacity, frame.bgra.size()));
    } // buffer lock burada serbest bırakılır

    // ── OCR çalıştır (async → sync) ──────────────────────────────────────────
    winrt::Windows::Media::Ocr::OcrEngine engineCopy{ nullptr };
    {
        std::lock_guard lock(m_engineMutex);
        engineCopy = m_ocrEngine;
    }
    auto ocrResult = engineCopy.RecognizeAsync(bitmap).get();

    // ── 1) Ham satırları topla (rengi de tahmin ederek) ─────────────────────
    struct RawLine {
        std::wstring text; RECT bounds;
        uint8_t r, g, b; bool hasColor;       // yazı (ön plan) rengi
        uint8_t bgR, bgG, bgB; bool hasBgColor; // arka plan rengi
        int lineHeight;
    };
    std::vector<RawLine> rawLines;
    for (auto const& line : ocrResult.Lines())
    {
        std::wstring lineText;
        RECT         lineBounds{ INT_MAX, INT_MAX, INT_MIN, INT_MIN };

        for (auto const& word : line.Words())
        {
            auto rect   = word.BoundingRect();
            lineText   += std::wstring(word.Text()) + L" ";
            lineBounds.left   = std::min(lineBounds.left,   static_cast<long>(rect.X));
            lineBounds.top    = std::min(lineBounds.top,    static_cast<long>(rect.Y));
            lineBounds.right  = std::max(lineBounds.right,  static_cast<long>(rect.X + rect.Width));
            lineBounds.bottom = std::max(lineBounds.bottom, static_cast<long>(rect.Y + rect.Height));
        }
        if (!lineText.empty()) lineText.pop_back(); // sondaki boşluğu sil
        if (lineText.empty()) continue;

        RawLine rl;
        rl.text       = std::move(lineText);
        rl.bounds     = lineBounds;
        rl.lineHeight = lineBounds.bottom - lineBounds.top; // TEK satırın gerçek yüksekliği (birleştirmeden önce)
        // Hem yazı hem arka plan rengi TEK geçişte hesaplanıyor – bölge
        // oluşturma aşamasında ikinci bir EstimateRegionColors çağrısına
        // (dolayısıyla piksellerin ikinci kez taranmasına) gerek kalmıyor.
        EstimateRegionColors(frame, lineBounds, rl.r, rl.g, rl.b, rl.hasColor,
                              rl.bgR, rl.bgG, rl.bgB, rl.hasBgColor);
        rawLines.push_back(std::move(rl));
    }
    Logger::DebugF("OCR: Windows OCR motoru {} ham satır buldu (birleştirme öncesi).", rawLines.size());

    // Okuma sırası garantisi için Y konumuna göre sırala.
    std::sort(rawLines.begin(), rawLines.end(),
        [](const RawLine& a, const RawLine& b) { return a.bounds.top < b.bounds.top; });

    // ── 2) Ekranda kelime kaydırmasıyla ikiye/üçe/daha fazla bölünmüş TEK bir
    //    cümleyi birleştir. Windows OCR "satır" düzeyinde çalışır; sadece
    //    dikey mesafeye bakmak satır aralığı geniş olan metinlerde yetersiz
    //    kalıyor. Bunun yerine DİLBİLGİSEL sinyalleri esas alıyoruz:
    //      a) Önceki satır cümle sonu noktalamasıyla (. ! ? …) BİTMİYORSA,
    //      b) Sonraki satır KÜÇÜK harfle başlıyorsa (yeni başlık/cümle
    //         genelde büyük harfle başlar; küçük harf = devam ediyor sinyali),
    //      c) İkisinin tahmini yazı rengi BELİRGİN ŞEKİLDE FARKLI DEĞİLSE
    //         (renk farklıysa, mesafe/noktalama ne derse desin ayrı cümledir),
    //    o zaman aynı paragrafın devamı kabul edilip birleştirilir. Dikey
    //    mesafe yalnızca çok uzak (alakasız) bloklarla yanlışlıkla
    //    birleşmeyi önleyen gevşek bir güvenlik sınırı olarak kullanılıyor.
    std::vector<RawLine> merged;
    for (auto& rl : rawLines)
    {
        if (!merged.empty())
        {
            RawLine& prev       = merged.back();
            int      prevHeight = prev.bounds.bottom - prev.bounds.top;
            int      gap        = rl.bounds.top - prev.bounds.bottom;

            // HATA DÜZELTMESİ (gözlemlenen somut örnek: yan yana duran "App
            // Store" ve "Google Play" rozetlerinin metinleri "App Store ogle
            // Play" gibi TEK bir cümleymiş gibi birleştiriliyordu): `gap`
            // NEGATİF olduğunda (iki satırın dikey aralığı ÇAKIŞIYORSA) bu
            // aslında satırların ALT ALTA değil YAN YANA (aynı satırda, iki
            // ayrı UI öğesinde) olduğu anlamına gelir – ki bu durumda
            // `gap < prevHeight * 3` kontrolü herhangi bir negatif sayı için
            // ZATEN her zaman doğru çıkıyordu, yani bu iki durumu hiç ayırt
            // etmiyordu. Gerçek bir kelime-kaydırmalı paragraf devamında
            // ikinci satır her zaman BİRİNCİNİN ALTINDA başlar (gap ~0 veya
            // pozitif); belirgin biçimde NEGATİF bir gap (küçük bir OCR kutu
            // gürültüsü payının – birkaç piksel – ÖTESİNDE) satırların aynı
            // satırda yan yana durduğunu, dolayısıyla FARKLI, İLGİSİZ birer
            // öğe olduğunu gösterir ve asla birleştirilmemelidir.
            constexpr int kMaxOverlapNoise = 4; // px – OCR kutu sınırı gürültüsü payı
            bool sameRowSideBySide = gap < -kMaxOverlapNoise;

            bool gapReasonable  = !sameRowSideBySide && (prevHeight <= 0 || gap < prevHeight * 3);
            bool prevIncomplete = !EndsWithSentenceTerminator(prev.text);
            bool looksContinuation = StartsWithLowercase(rl.text);
            bool colorBlocksMerge  = prev.hasColor && rl.hasColor &&
                ColorsSignificantlyDifferent(prev.r, prev.g, prev.b, rl.r, rl.g, rl.b);

            // EK KATMAN (kullanıcı önerisi): satırların ARASINDA KALAN
            // boşluğun kendi rengini örnekle. Bu boşluk her iki satırın
            // KENDİ arka plan rengine göre de belirgin şekilde FARKLIYSA
            // (örn. iki farklı renkli panel arasında kalan sayfa arka
            // planı gibi), bu satırların farklı KAPSAYICILARA ait olduğunu
            // gösterir – geometrik "aynı satırda mı" kontrolü bunu
            // YAKALAYAMAZ, çünkü bu durumda satırlar gerçekten ALT ALTA
            // olabilir (gap pozitif), sadece aralarında görsel bir sınır
            // vardır. Kasıtlı olarak HER İKİSİNDEN de (yalnızca birinden
            // değil) belirgin farklı olmasını arıyoruz – tek taraflı fark,
            // kutu kenarındaki hafif gradyan/anti-aliasing'den kaynaklanan
            // gürültü olabilir; gerçek bir konteyner sınırı ise boşluk her
            // iki tarafa göre de farklı olur.
            bool gapBgBlocksMerge = false;
            if (!sameRowSideBySide && gap >= 3 && prev.hasBgColor && rl.hasBgColor)
            {
                RECT stripRect{
                    std::min(prev.bounds.left, rl.bounds.left), prev.bounds.bottom,
                    std::max(prev.bounds.right, rl.bounds.right), rl.bounds.top
                };
                uint8_t stripR{}, stripG{}, stripB{};
                if (SampleStripAverageColor(frame, stripRect, stripR, stripG, stripB))
                {
                    bool differsFromPrev = ColorsSignificantlyDifferent(
                        stripR, stripG, stripB, prev.bgR, prev.bgG, prev.bgB);
                    bool differsFromNext = ColorsSignificantlyDifferent(
                        stripR, stripG, stripB, rl.bgR, rl.bgG, rl.bgB);
                    gapBgBlocksMerge = differsFromPrev && differsFromNext;
                }
            }

            // AYNI YÖNTEMİN YATAY (SÜTUN) HÂLİ – kullanıcı doğru tespit etti:
            // "iki blok arasındaki boşluğun rengini örnekle" fikri satıra
            // ÖZGÜ değil. `sameRowSideBySide` yalnızca satırlar DİKEYDE
            // ÖNEMLİ ÖLÇÜDE ÇAKIŞTIĞINDA (tam olarak aynı satırda) devreye
            // giriyordu; ama iki blok birbirine göre hafifçe kaydırılmış
            // (örn. bir sütun diğerinden biraz daha yukarıda/aşağıda
            // başlıyor) olup YİNE DE yan yana, X aralıkları örtüşmeyen ayrı
            // sütunlar olabilir – bu durumda dikey gap pozitif çıkıp sanki
            // "alt alta" gibi görünebilir. Bu yüzden X aralıkları GERÇEKTEN
            // AYRIKSA (disjointX), aradaki DİKEY şeridi (bu sefer X ekseninde
            // boşluk) örnekleyip aynı çift-taraflı-fark testini uyguluyoruz.
            bool horizontalGapBlocksMerge = false;
            {
                bool disjointX = false;
                int  stripLeft = 0, stripRight = 0;
                if (rl.bounds.left >= prev.bounds.right)
                { stripLeft = prev.bounds.right; stripRight = rl.bounds.left; disjointX = true; }
                else if (prev.bounds.left >= rl.bounds.right)
                { stripLeft = rl.bounds.right; stripRight = prev.bounds.left; disjointX = true; }

                if (disjointX && (stripRight - stripLeft) >= 3 && prev.hasBgColor && rl.hasBgColor)
                {
                    RECT stripRect{
                        stripLeft, std::min(prev.bounds.top, rl.bounds.top),
                        stripRight, std::max(prev.bounds.bottom, rl.bounds.bottom)
                    };
                    uint8_t stripR{}, stripG{}, stripB{};
                    if (SampleStripAverageColor(frame, stripRect, stripR, stripG, stripB))
                    {
                        bool differsFromPrev = ColorsSignificantlyDifferent(
                            stripR, stripG, stripB, prev.bgR, prev.bgG, prev.bgB);
                        bool differsFromNext = ColorsSignificantlyDifferent(
                            stripR, stripG, stripB, rl.bgR, rl.bgG, rl.bgB);
                        horizontalGapBlocksMerge = differsFromPrev && differsFromNext;
                    }
                }
            }

            if (sameRowSideBySide)
                Logger::DebugF("OCR: birleştirme engellendi – satırlar dikeyde çakışıyor "
                                "(yan yana/farklı öğeler), gap={}.", gap);
            if (gapBgBlocksMerge)
                Logger::Debug("OCR: birleştirme engellendi – satırlar arasındaki (dikey) "
                               "boşluk her ikisinin de arka planından belirgin şekilde "
                               "farklı (farklı konteyner/panel sınırı).");
            if (horizontalGapBlocksMerge)
                Logger::Debug("OCR: birleştirme engellendi – bloklar arasındaki (yatay) "
                               "boşluk her ikisinin de arka planından belirgin şekilde "
                               "farklı (ayrı sütun/blok sınırı).");

            if (gapReasonable && prevIncomplete && looksContinuation &&
                !colorBlocksMerge && !gapBgBlocksMerge && !horizontalGapBlocksMerge)
            {
                prev.text += L" " + rl.text;
                prev.bounds.left   = std::min(prev.bounds.left,   rl.bounds.left);
                prev.bounds.top    = std::min(prev.bounds.top,    rl.bounds.top);
                prev.bounds.right  = std::max(prev.bounds.right,  rl.bounds.right);
                prev.bounds.bottom = std::max(prev.bounds.bottom, rl.bounds.bottom);
                // NOT: prev.lineHeight kasıtlı olarak prev.bounds'tan DEĞİL,
                // birleştirilen tekil satırların ortalamasından hesaplanıyor.
                // Aksi halde (birleşmiş kutunun toplam yüksekliği kullanılırsa)
                // uzun, çok satıra yayılan bir cümlenin yazı boyutu tahmini
                // gerçek boyutun kat kat üzerine çıkar (rapor edilen hata).
                prev.lineHeight = (prev.lineHeight + rl.lineHeight) / 2;
                // Arka plan rengini de aynı şekilde taşı (birleşmiş kutunun
                // tamamını yeniden taramak yerine, birleştirilen satırların
                // arka plan tahminlerinin ortalaması kullanılıyor).
                if (prev.hasBgColor && rl.hasBgColor)
                {
                    prev.bgR = static_cast<uint8_t>((prev.bgR + rl.bgR) / 2);
                    prev.bgG = static_cast<uint8_t>((prev.bgG + rl.bgG) / 2);
                    prev.bgB = static_cast<uint8_t>((prev.bgB + rl.bgB) / 2);
                }
                else if (rl.hasBgColor)
                {
                    prev.bgR = rl.bgR; prev.bgG = rl.bgG; prev.bgB = rl.bgB;
                    prev.hasBgColor = true;
                }
                continue;
            }

            if (colorBlocksMerge)
                Logger::DebugF("OCR: birleştirme engellendi – yazı rengi belirgin şekilde farklı "
                                "(önceki=({},{},{}), sonraki=({},{},{})).",
                    prev.r, prev.g, prev.b, rl.r, rl.g, rl.b);
        }
        merged.push_back(rl);
    }
    if (merged.size() != rawLines.size())
        Logger::DebugF("OCR: {} ham satır, cümle birleştirmesiyle {} bölgeye indirgendi.",
            rawLines.size(), merged.size());

    // ── 3) Kalite filtrelerini birleştirilmiş paragraflara uygula ────────────
    std::vector<OcrRegion> regions;
    int lineIndex = 0;
    for (auto& rl : merged)
    {
        ++lineIndex;
        const std::wstring& lineText   = rl.text;
        RECT                lineBounds = rl.bounds;

        bool  passesQuality = PassesQualityFilter(lineText);
        float englishRatio  = ComputeEnglishWordRatio(lineText);

        std::string lineTextUtf8 = WideToUtf8(lineText);
        Logger::DebugF("OCR: bölge {} – \"{}\" (uzunluk={}, kaliteFiltresi={}, ingilizceOran={:.2f})",
            lineIndex, lineTextUtf8, lineText.size(), passesQuality ? "geçti" : "ELENDİ", englishRatio);

        if (!passesQuality)
        {
            Logger::DebugF("OCR: bölge {} elendi – minCharCount={}, minWordCount={} eşiğini karşılamadı.",
                lineIndex, m_cfg.minCharCount, m_cfg.minWordCount);
            continue;
        }
        // Bu oran-tabanlı filtre yalnızca kaynak dil AÇIKÇA İngilizce
        // seçildiğinde uygulanıyor. "Otomatik Algıla" ya da başka bir dil
        // seçiliyken bu filtre geçerli, yabancı-alfabeli metni yanlışlıkla
        // "gürültü" sayıp elerdi.
        if (m_srcLang == L"en-US" && englishRatio < 0.6f)
        {
            Logger::DebugF("OCR: bölge {} elendi – İngilizce kelime oranı {:.2f} < 0.60.",
                lineIndex, englishRatio);
            continue;
        }

        // ── Yazı boyutu/rengi VE arka plan rengi ──────────────────────────────
        // Renkler burada YENİDEN hesaplanmıyor – zaten ham satır toplama
        // aşamasında (rl için) tek geçişte hesaplanıp birleştirme sırasında
        // taşınmıştı. İkinci bir piksel taraması (performans için) burada
        // KASITLI olarak yapılmıyor.
        uint8_t estFgR = rl.r,   estFgG = rl.g,   estFgB = rl.b;
        uint8_t estBgR = rl.bgR, estBgG = rl.bgG, estBgB = rl.bgB;
        bool    hasFgColor = rl.hasColor, hasBgColor = rl.hasBgColor;

        // KRİTİK: rl.bounds.bottom-rl.bounds.top DEĞİL, rl.lineHeight kullanılıyor.
        // Birleştirilmiş (çok satırlı) bir bölgede rl.bounds artık TÜM
        // paragrafın (birden fazla ekran satırının) birleşik/union
        // yüksekliğidir – onu kullanmak, uzun/çok satırlı cümlelerde yazı
        // boyutunun gerçek boyutun kat kat üzerinde tahmin edilmesine yol
        // açıyordu. rl.lineHeight, birleştirilen TEKİL satırların
        // (birleştirme öncesi) yüksekliklerinin ortalamasını tutar.
        int   textPxHeight = rl.lineHeight;
        // NOT: Burada artık DirectWrite font boyutuna (em) SABİT bir çarpanla
        // (önce 0.75, önce onun öncesinde 0.87 vb.) tahmin YAPMIYORUZ. Bu tür
        // sabit çarpanlar hiçbir zaman tüm satırlarda tutarlı çalışmadı, çünkü
        // OCR'ın tespit ettiği piksel yüksekliği (ascender'dan descender'a
        // gerçek mürekkep aralığı) ile DirectWrite em boyutu arasındaki oran
        // FONTA GÖRE DEĞİŞİR (örn. Arial'de ascent+descent ≈ em'in %112'si;
        // kalın/normal ayrımı da oranı kaydırır). Sabit bir çarpanla küçültmek
        // (0.75) em boyutunu gerçek em boyutunun bile altına düşürüyordu –
        // bu yüzden çeviri metni orijinalinden belirgin küçük çıkıyordu.
        //
        // Bunun yerine ham piksel yüksekliğini OLDUĞU GİBİ taşıyoruz;
        // DirectWrite em boyutuna dönüşüm artık OverlayEngine::DrawText
        // içinde, o an kullanılan fontun GERÇEK ascent/descent metriklerinden
        // (IDWriteFontFace::GetMetrics) hesaplanıyor. Böylece hangi font
        // seçilirse seçilsin oran doğru çıkar, elle kalibrasyon gerekmez.
        float estFontSize = std::clamp(static_cast<float>(textPxHeight), 6.f, 300.f);

        Logger::DebugF("OCR: bölge {} algılanan görünüm – yazı={} ({},{},{}), "
                        "arkaPlan={} ({},{},{}), fontBoyutu={:.0f}px",
            lineIndex, hasFgColor ? "OK" : "varsayılan", estFgR, estFgG, estFgB,
            hasBgColor ? "OK" : "varsayılan", estBgR, estBgG, estBgB, estFontSize);

        // Monitörün sanal masaüstü ofsetini ekle
        lineBounds.left   += frame.virtualRect.left;
        lineBounds.right  += frame.virtualRect.left;
        lineBounds.top    += frame.virtualRect.top;
        lineBounds.bottom += frame.virtualRect.top;

        OcrRegion region;
        region.id         = NewID();
        region.bounds     = lineBounds;
        region.text       = lineText;
        region.confidence = 1.0f; // Windows OCR kelime başına güven skoru sunmuyor
        region.hasDetectedColor   = hasFgColor;
        region.detectedColor      = hasFgColor ? RGB(estFgR, estFgG, estFgB) : COLORREF{};
        region.hasDetectedBgColor = hasBgColor;
        region.detectedBgColor    = hasBgColor ? RGB(estBgR, estBgG, estBgB) : COLORREF{};
        region.detectedFontSize   = estFontSize;

        regions.push_back(std::move(region));
    }
    return regions;
}

bool OCREngine::PassesQualityFilter(const std::wstring& text) const
{
    if (static_cast<int>(text.size()) < m_cfg.minCharCount) return false;
    int words = 1 + static_cast<int>(std::count(text.begin(), text.end(), L' '));
    return words >= m_cfg.minWordCount;
}

float OCREngine::ComputeEnglishWordRatio(const std::wstring& text) const
{
    int total = 0, english = 0;
    std::wistringstream iss(text);
    std::wstring word;
    while (iss >> word)
    {
        // Kelimenin başındaki/sonundaki noktalama işaretlerini (. , ! ? : ; " ' ( ) vb.)
        // at, yalnızca asıl alfabetik gövdeye bak. Bu olmadan "Cross." veya
        // "(permute):" gibi noktalamalı TEK kelimeler yanlışlıkla "İngilizce
        // değil" sayılıp tüm satır/bölge gereksiz yere eleniyordu.
        size_t start = 0, end = word.size();
        while (start < end && !std::iswalnum(static_cast<wint_t>(word[start]))) ++start;
        while (end > start && !std::iswalnum(static_cast<wint_t>(word[end - 1]))) --end;

        if (start == end) continue; // salt noktalama/simge – ne say ne cezalandır

        ++total;
        std::wstring core = word.substr(start, end - start);
        // Yalnızca salt alfasayısal değil; kelime İÇİNDEKİ kesme işareti
        // (What's, don't, it's) ve tire (in-game, well-known) de İngilizce'de
        // tamamen normaldir. Bunları da kabul etmezsek "What's"/"in-game" gibi
        // gayet geçerli kelimeler yanlışlıkla "İngilizce değil" sayılıp tüm
        // satır/bölge (dolayısıyla çeviri) gereksiz yere eleniyordu.
        bool isAscii = std::all_of(core.begin(), core.end(), [](wchar_t c){
            return c < 128 && (std::isalnum(static_cast<unsigned char>(c)) || c == L'\'' || c == L'-');
        });
        if (isAscii) ++english;
    }
    return total == 0 ? 0.f : static_cast<float>(english) / total;
}

// (HashRegion kaldırıldı: ürettiği pixelHash hiçbir yerde okunmuyordu –
// saf hesaplama israfıydı. Değişiklik tespiti zaten OCR metni + IoU
// eşleşmesiyle yapılıyor; ekran-değişmedi kontrolü ise artık daha büyük
// bir kazanç için OCR'dan ÖNCE, kare düzeyinde yapılıyor – bkz.
// Application::HashFrameSample.)

void OCREngine::Restart()
{
    m_running.store(false);
    m_queueCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_running.store(true);
    m_thread = std::thread(&OCREngine::WorkerLoop, this);
    Logger::Info("OCREngine: yeniden başlatıldı.");
}
