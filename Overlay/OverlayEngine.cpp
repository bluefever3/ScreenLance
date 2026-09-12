#include "../Utils/pch.h"
#include "OverlayEngine.h"
#include "../Utils/Logger.h"
#include <chrono>

static std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ─── InkBoundsTextRenderer ───────────────────────────────────────────────────
// Bir IDWriteTextLayout'u GERÇEKTEN "çizdirip" (ekrana hiçbir şey basmadan,
// yalnızca glyph geometrisini toplayarak), o metnin GERÇEK vektör anahat
// (glyph outline) sınırlarını ölçer. Bkz. MeasureInkMetrics yorumu:
// DWRITE_OVERHANG_METRICS bunun için YANLIŞ araçtı (mürekkebin fontun TASARIM
// kutusunu aşıp aşmadığını ölçer, normal metin hep içeride kaldığından her
// zaman ~0 dönüyordu). Bu sınıf, IDWriteTextLayout::Draw() üzerinden gelen
// her glyph run için IDWriteFontFace::GetGlyphRunOutline ile GERÇEK vektör
// anahatları alıp bir ID2D1PathGeometry'ye yazar ve GetBounds() ile kesin
// (tight) dikey sınırları bulur – hangi karakterler geçerse geçsin (yalnız
// büyük harf, yalnız küçük harf, sarkan/çıkıntılı harfli vb.) doğru sonuç
// verir.
//
// NOT (COM ömür yönetimi): Bu nesne BİLEREK yığın (stack) üzerinde
// oluşturuluyor ve tek bir senkron IDWriteTextLayout::Draw() çağrısı
// süresince yaşıyor; bu yüzden AddRef/Release GERÇEK referans sayımı
// yapmıyor (asla delete çağırmıyor) – yıkımı normal C++ kapsam (scope) sonu
// hallediyor. Bu, tek seferlik/atılabilir geri çağırma (callback) nesneleri
// için bilinen ve güvenli bir kalıptır.
class InkBoundsTextRenderer : public IDWriteTextRenderer
{
public:
    explicit InkBoundsTextRenderer(ID2D1Factory1* factory) : m_factory(factory) {}

    float MinY()   const { return m_minY; }
    float MaxY()   const { return m_maxY; }
    bool  HasInk() const { return m_hasInk; }

    // ── IUnknown ──
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer))
        {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; } // bkz. yukarıdaki NOT

    // ── IDWritePixelSnapping ──
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override
    { *isDisabled = TRUE; return S_OK; }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    { *transform = DWRITE_MATRIX{ 1.f, 0.f, 0.f, 1.f, 0.f, 0.f }; return S_OK; }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
    { *pixelsPerDip = 1.f; return S_OK; }

    // ── IDWriteTextRenderer ──
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT /*baselineOriginX*/, FLOAT baselineOriginY, DWRITE_MEASURING_MODE,
        DWRITE_GLYPH_RUN const* glyphRun, DWRITE_GLYPH_RUN_DESCRIPTION const*, IUnknown*) override
    {
        if (!glyphRun || !glyphRun->fontFace || glyphRun->glyphCount == 0 || !m_factory)
            return S_OK;

        ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(m_factory->CreatePathGeometry(geom.put()))) return S_OK;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(sink.put()))) return S_OK;

        HRESULT hr = glyphRun->fontFace->GetGlyphRunOutline(
            glyphRun->fontEmSize, glyphRun->glyphIndices, glyphRun->glyphAdvances,
            glyphRun->glyphOffsets, glyphRun->glyphCount, glyphRun->isSideways,
            (glyphRun->bidiLevel % 2) != 0, sink.get());
        sink->Close();
        if (FAILED(hr)) return S_OK;

        D2D1_RECT_F bounds{};
        if (SUCCEEDED(geom->GetBounds(nullptr, &bounds)) && bounds.bottom > bounds.top)
        {
            m_minY = (std::min)(m_minY, baselineOriginY + bounds.top);
            m_maxY = (std::max)(m_maxY, baselineOriginY + bounds.bottom);
            m_hasInk = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override
    { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override
    { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override
    { return S_OK; }

private:
    ID2D1Factory1* m_factory{ nullptr };
    float m_minY{ (std::numeric_limits<float>::max)() };
    float m_maxY{ (std::numeric_limits<float>::lowest)() };
    bool  m_hasInk{ false };
};

OverlayEngine::OverlayEngine(const OverlayConfig& cfg, HINSTANCE hInstance)
    : m_cfg(cfg), m_hInstance(hInstance)
{}

OverlayEngine::~OverlayEngine() { Shutdown(); }

HRESULT OverlayEngine::Init(const RECT& virtualDesktopRect)
{
    m_virtualRect = virtualDesktopRect;

    // ── Pencere sınıfını kaydet ───────────────────────────────────────────────
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    int w = virtualDesktopRect.right  - virtualDesktopRect.left;
    int h = virtualDesktopRect.bottom - virtualDesktopRect.top;

    // ── Şeffaf, tıkla-geç, her zaman üstte pencere ───────────────────────────
    m_hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName, L"",
        WS_POPUP,
        virtualDesktopRect.left, virtualDesktopRect.top, w, h,
        nullptr, nullptr, m_hInstance, this);

    if (!m_hwnd)
    {
        Logger::Error("OverlayEngine: CreateWindow başarısız.");
        return E_FAIL;
    }
    ::SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

    // KRİTİK: Overlay penceresi HİÇBİR ekran yakalama API'sine görünmemeli
    // (ne Desktop Duplication, ne GDI BitBlt). Bu olmadan, overlay'in
    // kendi çevirisi yanlışlıkla yakalanıp tekrar OCR'a girebilir
    // (İngilizce → Türkçe → Türkçe'yi tekrar OCR → sonsuz döngü riski).
    // Windows 10 2004+ (build 19041+) gerektirir; hedef platform olan
    // Windows 11'de her zaman mevcuttur.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    if (!::SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
        Logger::WarningF("OverlayEngine: SetWindowDisplayAffinity başarısız (hata {}). "
                         "Overlay capture'lardan gizlenemedi.", ::GetLastError());

    HRESULT hr = CreateDeviceResources();
    if (FAILED(hr)) return hr;

    ::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(m_hwnd);

    // ── Render thread ─────────────────────────────────────────────────────────
    m_running.store(true);
    m_renderThread = std::thread([this]
    {
        using clock    = std::chrono::steady_clock;
        auto interval  = std::chrono::milliseconds(1000 / 60);
        while (m_running.load(std::memory_order_relaxed))
        {
            auto t0 = clock::now();
            {
                std::lock_guard lock(m_regionMutex);
                if (m_dirty) { RenderFrame(); m_dirty = false; }
            }
            auto elapsed = clock::now() - t0;
            if (elapsed < interval)
                std::this_thread::sleep_for(interval - elapsed);
        }
    });

    Logger::Info("OverlayEngine: hazır.");
    return S_OK;
}

// GERÇEK font metriklerini (ascent/descent, em başına oran) okur. Sabit
// tahmini çarpanlar (0.75, 0.87 vb.) yerine bunu kullanmamızın sebebi: bu
// oran fonta göre değişir (örn. Arial'de ascent+descent ≈ em'in %112'si,
// başka bir fontta %100 ya da %120 olabilir), dolayısıyla hiçbir sabit
// çarpan tüm fontlarda/tüm satırlarda doğru sonuç vermez.
void OverlayEngine::RefreshFontMetricRatios()
{
    if (!m_dwFactory) return;

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(m_dwFactory->GetSystemFontCollection(collection.put())))
    {
        Logger::Warning("OverlayEngine: sistem font koleksiyonu alınamadı, varsayılan ascent/descent kullanılacak.");
        return;
    }

    UINT32 index = 0; BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(m_cfg.fontFamily.c_str(), &index, &exists)) || !exists)
    {
        Logger::WarningF("OverlayEngine: font ailesi \"{}\" bulunamadı, varsayılan ascent/descent kullanılacak.",
            WideToUtf8(m_cfg.fontFamily));
        return;
    }

    ComPtr<IDWriteFontFamily> family;
    if (FAILED(collection->GetFontFamily(index, family.put()))) return;

    ComPtr<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(
            m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put())))
        return;

    ComPtr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(face.put()))) return;

    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    if (metrics.designUnitsPerEm == 0) return;

    m_fontAscentRatio  = static_cast<float>(metrics.ascent)  / metrics.designUnitsPerEm;
    m_fontDescentRatio = static_cast<float>(metrics.descent) / metrics.designUnitsPerEm;

    Logger::DebugF("OverlayEngine: font metrikleri güncellendi ({}): ascentOran={:.3f}, descentOran={:.3f}",
        WideToUtf8(m_cfg.fontFamily), m_fontAscentRatio, m_fontDescentRatio);
}

// Belirli bir metni referans bir font boyutunda GERÇEKTEN DirectWrite ile
// dizip GERÇEK mürekkep (ink) yüksekliğini VE "üst boşluk" oranını TEK
// GEÇİŞTE ölçer:
//   outHeightRatio   – mürekkep yüksekliği / referans font boyutu (BOYUT
//                      hesaplaması için; bkz. OverlayEngine.h yorumu).
//   outTopInsetRatio – mürekkebin, DirectWrite'ın doğal (satır üstten
//                      hizalı) layout kutusunun TEPESİNE göre ne kadar
//                      AŞAĞIDA başladığı / referans font boyutu (dikey
//                      HİZALAMA için).
// İkisini de fontun genel ascent/descent oranı yerine BUNUNLA
// hesaplamamızın sebebi aynı: OCR'ın ölçtüğü kutu, satırda GEÇEN GERÇEK
// karakterlere göre değişir. Özellikle outTopInsetRatio için fontun genel
// ascent oranını (aksan/diyakritik gibi en uç durumları da kapsayan,
// TÜM font için tanımlı değer) kullanmak KRİTİK bir hataydı: örneğin
// tamamen büyük harfli bir metinde gerçek mürekkep yalnızca cap-height
// kadardır, ama genel ascent oranı bundan belirgin şekilde büyüktür – bu
// da taban çizgisini gereğinden fazla aşağı iterek metnin kutunun
// tepesinden ÖNEMLİ ÖLÇÜDE aşağıda başlamasına (gözle "üstten boşluk"
// olarak görünen boşluğa) yol açıyordu.
//
// GERÇEK vektör glyph anahatlarını ölçüyoruz (bkz. InkBoundsTextRenderer
// yorumu). DWRITE_OVERHANG_METRICS'in neden YANLIŞ olduğu (önceki
// denemede kullanılmıştı): o, mürekkebin fontun TASARIM kutusunu
// (ascent+descent) aşıp aşmadığını ölçer; normal metin bu kutunun içinde
// kaldığından her zaman ~0 dönüyor ve yanlışlıkla genel ascent+descent
// oranıyla aynı (hatalı) sonucu veriyordu.
bool OverlayEngine::MeasureInkMetrics(const std::wstring& text, float& outHeightRatio, float& outTopInsetRatio) const
{
    outHeightRatio   = 0.f;
    outTopInsetRatio = 0.f;

    constexpr float kRefSize = 200.f;
    if (!m_dwFactory || !m_d2dFactory || text.empty()) return false;

    ComPtr<IDWriteTextFormat> refFormat;
    HRESULT hr = m_dwFactory->CreateTextFormat(
        m_cfg.fontFamily.c_str(), nullptr,
        m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        kRefSize, L"tr-TR", refFormat.put());
    if (FAILED(hr)) return false;
    refFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> layout;
    hr = m_dwFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
        refFormat.get(), 100000.f, 100000.f, layout.put());
    if (FAILED(hr)) return false;

    InkBoundsTextRenderer renderer(m_d2dFactory.get());
    if (FAILED(layout->Draw(nullptr, &renderer, 0.f, 0.f)) || !renderer.HasInk())
        return false;

    float inkHeight = renderer.MaxY() - renderer.MinY();
    if (inkHeight <= 0.f) return false;

    outHeightRatio   = inkHeight / kRefSize;
    // renderer.MinY() negatif de olabilir (nadiren – çok yüksek aksan/
    // diyakritikli karakterlerde mürekkep, DirectWrite'ın "doğal" layout
    // kutusunun biraz üstüne taşabilir); bu durumda üstteki kaydırma
    // otomatik olarak ters yöne (aşağı) döner, ki bu da doğrudur.
    outTopInsetRatio = renderer.MinY() / kRefSize;
    return true;
}

// Bkz. OverlayEngine.h'daki yorum. Bu fonksiyon, DrawRegion() içinde BİR KEZ
// çağrılır ve sonucu hem arka plan dolgusu hem de metin çizimi için ORTAK
// olarak kullanılır – böylece ikisi asla farklı kutular kullanmaz.
void OverlayEngine::ComputeTextLayoutInfo(const TranslatedRegion& r, float boxLeft, float boxWidth, float maxRight,
                                           float& outFontSize, float& outWidth, float& outHeight, bool& outSingleLine,
                                           float& outTopInsetRatio) const
{
    outFontSize      = m_cfg.fontSize;
    outWidth         = boxWidth;
    outHeight        = 0.f;   // 0 = çağıran taraf orijinal OCR kutu yüksekliğini kullansın
    outSingleLine    = true;
    outTopInsetRatio = 0.f;

    if (r.detectedFontSize <= 0.f || !m_dwFactory) return;

    float targetPxHeight = r.detectedFontSize;

    // ÖNCELİK: bu SATIRDA GEÇEN GERÇEK karakterlere göre ölçülen oranlar
    // (r.originalText – OCR'ın çeviriden ÖNCE gördüğü metin). Fontun genel
    // ascent+descent oranı yalnızca bu ölçüm başarısız olursa yedek olur.
    float heightRatio = 0.f;
    if (MeasureInkMetrics(r.originalText, heightRatio, outTopInsetRatio))
    {
        outFontSize = std::clamp(targetPxHeight / heightRatio, 6.f, 300.f);
    }
    else
    {
        float fallbackRatio = m_fontAscentRatio + m_fontDescentRatio;
        outFontSize = std::clamp((fallbackRatio > 0.01f) ? (targetPxHeight / fallbackRatio) : targetPxHeight,
            6.f, 300.f);
        // Yedek yolda "üst boşluk" için de fontun genel ascent oranını
        // kullanıyoruz (içerik-duyarlı ölçüm yoksa elimizdeki en iyi tahmin).
        outTopInsetRatio = m_fontAscentRatio;
    }
    outHeight = targetPxHeight;

    if (r.translatedText.empty() || !m_dwFactory) return;

    ComPtr<IDWriteTextFormat> probeFormat;
    if (FAILED(m_dwFactory->CreateTextFormat(
            m_cfg.fontFamily.c_str(), nullptr,
            m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            outFontSize, L"tr-TR", probeFormat.put())))
        return;
    probeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 1) ÖNCE: çeviri metni SINIRSIZ genişlikte tek satırda ne kadar yer
    //    kaplardı, ölç. OCR'ın dar kutu genişliği "mevcut TÜM alan" değil,
    //    yalnızca ORİJİNAL metnin kapladığı alandır – ekranda hâlâ boş yer
    //    olabilir (bkz. üstteki yorum).
    ComPtr<IDWriteTextLayout> naturalLayout;
    float naturalWidth = boxWidth;
    if (SUCCEEDED(m_dwFactory->CreateTextLayout(r.translatedText.c_str(),
            static_cast<UINT32>(r.translatedText.size()), probeFormat.get(),
            100000.f, 100000.f, naturalLayout.put())))
    {
        DWRITE_TEXT_METRICS ntm{};
        if (SUCCEEDED(naturalLayout->GetMetrics(&ntm)))
            naturalWidth = ntm.width;
    }

    float availableWidth = (maxRight > boxLeft) ? (maxRight - boxLeft) : boxWidth;

    if (naturalWidth <= availableWidth)
    {
        // Tek satıra sığıyor (gerektiğinde kutuyu yatayda büyüterek) –
        // satır atlamaya HİÇ gerek yok, font boyutu da orijinaliyle aynı
        // kalıyor. Küçük bir güvenlik payı (birkaç piksel) ekliyoruz:
        // naturalWidth, NO_WRAP bir prob ile ölçüldü; asıl çizimde
        // kullanılan format ile arasında yuvarlama/kerning kaynaklı çok
        // ufak bir fark bile metnin gereksiz yere satır atlamasına yol
        // açabiliyordu (özellikle kutunun mevcut genişliği zaten doğal
        // genişliğe çok yakınken).
        constexpr float kWidthSafetyMargin = 4.f;
        outWidth = (std::max)(boxWidth, naturalWidth + kWidthSafetyMargin);

        // KRİTİK DÜZELTME (font büyük görünüyor + arka planı taşıyor):
        // outHeight'ı buraya kadar hep targetPxHeight (OCR'ın ölçtüğü HAM
        // mürekkep yüksekliği, örn. 66px) olarak bırakıyorduk. Ama
        // outFontSize genelde bundan BELİRGİN ÖLÇÜDE büyüktür (cap-height
        // oranı ~0.7 olduğundan, 66px hedef için ~91px font boyutu
        // gerekebiliyor – bkz. üstteki heightRatio hesabı) ve DirectWrite'ın
        // o font boyutunda GERÇEKTEN ihtiyaç duyduğu doğal satır yüksekliği
        // (ascent+descent) ink yüksekliğinden daha da fazladır. Kutu (ve
        // dolayısıyla arka plan dolgusu) yalnızca 66px yüksekliğinde
        // kalınca, gerçek satır yüksekliği bunu aşıp hem metin "olması
        // gerekenden büyük" görünüyor hem de arka planın altına/üstüne
        // taşıyordu. Düzeltme: ntm (yukarıda ZATEN ölçülen, bu fontSize'da
        // bu ÇEVİRİ METNİNİN doğal tek-satır layout'u) içindeki .height
        // alanını da kullanıp kutuyu, DirectWrite'ın gerçekte ihtiyaç
        // duyacağı yükseklikten KESİNLİKLE daha küçük OLMAYACAK şekilde
        // büyütüyoruz. max() kullanıyoruz ki OCR'ın ölçtüğü yükseklik zaten
        // yeterliyse (nadiren, örn. çok küçük fontlarda) kutuyu gereksiz
        // büyütmeyelim.
        DWRITE_TEXT_METRICS ntm{};
        if (naturalLayout && SUCCEEDED(naturalLayout->GetMetrics(&ntm)) && ntm.height > 0.f)
            outHeight = (std::max)(targetPxHeight, ntm.height);
        return;
    }

    // 2) Genişletilmiş sınıra kadar bile SIĞMIYOR (gerçekten ekran/monitör
    //    kenarına dayanıyor, YA DA algılanan bir arka plan rengi olduğu için
    //    yatay büyümeye hiç izin verilmiyor). Sarmaya geçmeden ÖNCE, eğer
    //    bölgenin algılanmış bir arka plan rengi varsa (r.hasDetectedBgColor
    //    – yani muhtemelen sabit boyutlu bir düğme/rozet/panel), FONTU
    //    KADEMELİ OLARAK KÜÇÜLTEREK tek satıra sığdırmayı deniyoruz. Amaç:
    //    böyle dar/sabit yükseklikli UI öğelerinde kutunun dikeyde
    //    büyüyüp panelin GERÇEK sınırlarının dışına taşmasını (kullanıcının
    //    bildirdiği "gri alanda kalamıyor" sorunu) mümkün olduğunca önlemek.
    //    Düz metin üzerindeki (arka planı algılanmamış) bölgelerde bunu
    //    YAPMIYORUZ – orijinal davranış (satır atlama) korunuyor, çünkü
    //    orada zaten yatay büyüme serbest, font küçültmeye gerek yok.
    if (r.hasDetectedBgColor && availableWidth > 1.f && naturalWidth > availableWidth)
    {
        // ÖNCEKİ YAKLAŞIM (sabit %8'lik adımlarla %55 tabanına kadar deneme)
        // yeterince AGRESİF değildi: "PLATINUM CARD" -> "PLATINUM KARTI" gibi
        // neredeyse AYNI uzunlukta bir çeviri bile bazen gereken küçültme
        // oranını (%55 tabanının ÖTESİNDE) bulamayıp gereksiz yere 2 satıra
        // sarılıyordu – oysa gereken küçültme oranı DOĞRUDAN hesaplanabilir:
        // genişlik, font boyutuyla DOĞRU ORANTILI değişir (aynı metin/font
        // için), yani `availableWidth / naturalWidth` oranı bize TAM olarak
        // hangi ölçekte sığacağını söyler – kör adımlarla aramaya gerek yok.
        constexpr float kSafety       = 0.96f; // kerning/yuvarlama payı
        constexpr float kMinFontScale = 0.40f; // bunun altı okunaksız kabul edilir

        float requiredScale = std::clamp((availableWidth / naturalWidth) * kSafety, kMinFontScale, 1.f);
        float candidate = outFontSize * requiredScale;

        ComPtr<IDWriteTextFormat> tryFormat;
        if (SUCCEEDED(m_dwFactory->CreateTextFormat(
                m_cfg.fontFamily.c_str(), nullptr,
                m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                candidate, L"tr-TR", tryFormat.put())))
        {
            tryFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteTextLayout> tryLayout;
            if (SUCCEEDED(m_dwFactory->CreateTextLayout(r.translatedText.c_str(),
                    static_cast<UINT32>(r.translatedText.size()), tryFormat.get(),
                    100000.f, 100000.f, tryLayout.put())))
            {
                DWRITE_TEXT_METRICS ctm{};
                if (SUCCEEDED(tryLayout->GetMetrics(&ctm)) && ctm.width <= availableWidth)
                {
                    // Sığdı: küçültülmüş fontla tek satırda kal, hiç sarma/
                    // dikey büyüme yapma – panelin orijinal yüksekliği
                    // olabildiğince korunur.
                    constexpr float kWidthSafetyMargin = 4.f;
                    outFontSize = candidate;
                    outWidth    = (std::max)(boxWidth, ctm.width + kWidthSafetyMargin);
                    if (ctm.height > 0.f)
                        outHeight = (std::max)(targetPxHeight, ctm.height);
                    return;
                }
                // Doğrudan hesaplanan ölçek (kerning farkları yüzünden) tam
                // sığmadıysa, tabana (kMinFontScale) kadar bir kez daha dene
                // – hesaplanan oran zaten tabana YAKINSA bu ek denemenin
                // maliyeti ihmal edilebilir düzeydedir.
                if (requiredScale > kMinFontScale)
                {
                    float floorCandidate = outFontSize * kMinFontScale;
                    ComPtr<IDWriteTextFormat> floorFormat;
                    if (SUCCEEDED(m_dwFactory->CreateTextFormat(
                            m_cfg.fontFamily.c_str(), nullptr,
                            m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            floorCandidate, L"tr-TR", floorFormat.put())))
                    {
                        floorFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        ComPtr<IDWriteTextLayout> floorLayout;
                        if (SUCCEEDED(m_dwFactory->CreateTextLayout(r.translatedText.c_str(),
                                static_cast<UINT32>(r.translatedText.size()), floorFormat.get(),
                                100000.f, 100000.f, floorLayout.put())))
                        {
                            DWRITE_TEXT_METRICS ftm{};
                            if (SUCCEEDED(floorLayout->GetMetrics(&ftm)) && ftm.width <= availableWidth)
                            {
                                constexpr float kWidthSafetyMargin = 4.f;
                                outFontSize = floorCandidate;
                                outWidth    = (std::max)(boxWidth, ftm.width + kWidthSafetyMargin);
                                if (ftm.height > 0.f)
                                    outHeight = (std::max)(targetPxHeight, ftm.height);
                                return;
                            }
                        }
                    }
                }
            }
        }
        // Küçültme (taban dahil) yetmedi – aşağıdaki normal sarma mantığına,
        // ORİJİNAL (küçültülmemiş) font boyutuyla devam ediyoruz; en azından
        // kutu yatayda hiç taşmayacak.
    }

    // Genişletilmiş sınıra kadar bile SIĞMIYOR (gerçekten ekran/monitör
    // kenarına dayanıyor) – ancak BU durumda satırlara sarılır. Kutu
    // genişliğini mevcut azami genişliğe (availableWidth) çekip, o
    // genişlikte kaç satıra ihtiyaç olduğunu ölçüyoruz.
    //
    // KRİTİK HATA DÜZELTMESİ: buraya kadar `probeFormat` üzerinde
    // SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP) çağrılmıştı (bkz.
    // yukarıdaki satır ~348 – doğal/tek-satır GENİŞLİK ölçümü için
    // BİLEREK kapatılmıştı). AYNI format nesnesi burada `wrapLayout` için
    // de kullanılıyordu – yani "sarma" denemesi ASLA gerçek bir sarma
    // YAPAMIYORDU: DirectWrite, NO_WRAP olan bir formatla, verilen
    // maxWidth'i (availableWidth) basitçe GÖRMEZDEN GELİP metni TEK
    // SATIRDA YATAYDA TAŞIRARAK çiziyordu. Sonuç: tm.lineCount HER ZAMAN
    // 1 çıkıyor, aşağıdaki "if (tm.lineCount > 1)" bloğu HİÇBİR ZAMAN
    // çalışmıyor ve outHeight, OCR'ın ham (66px gibi) yüksekliğinde
    // kilitli kalıyordu – tam da "hem büyük görünüyor hem arka planını
    // aşıyor" şikâyetinin kök nedeni buydu (metin hem yatayda kutunun
    // dışına taşıyordu HEM DE kutu yüksekliği hiç büyümüyordu). Çözüm:
    // sarma denemesi için WRAP AÇIK yeni, AYRI bir format kullanmak.
    outWidth = availableWidth;

    ComPtr<IDWriteTextFormat> wrapFormat;
    if (FAILED(m_dwFactory->CreateTextFormat(
            m_cfg.fontFamily.c_str(), nullptr,
            m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            outFontSize, L"tr-TR", wrapFormat.put())))
        return;
    wrapFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); // <-- probeFormat'tan FARKLI olarak burada AÇIK

    ComPtr<IDWriteTextLayout> wrapLayout;
    if (FAILED(m_dwFactory->CreateTextLayout(r.translatedText.c_str(),
            static_cast<UINT32>(r.translatedText.size()), wrapFormat.get(),
            availableWidth, 100000.f, wrapLayout.put())))
        return;

    DWRITE_TEXT_METRICS tm{};
    if (FAILED(wrapLayout->GetMetrics(&tm))) return;

    // ÖNEMLİ: artık tm.lineCount GERÇEKTEN 1'den büyük çıkabiliyor (wrap
    // açık olduğu için). Ama tek satıra sığan durumda bile (tm.lineCount
    // == 1, örn. availableWidth aslında yeterliyse) outHeight'ı yine de
    // DirectWrite'ın bu font boyutunda GERÇEKTEN ihtiyaç duyduğu doğal
    // satır yüksekliğinden (tm.height) küçük BIRAKMIYORUZ – yukarıdaki
    // "naturalWidth <= availableWidth" dalındaki DÜZELTMEYLE aynı mantık,
    // burada da (font küçültme başarısız olup BU dala düşüldüğünde) aynı
    // sınıf hataya (kutu, gerçek font boyutuna göre çok küçük kalıp
    // taşma) düşmemek için gerekli.
    if (tm.height > 0.f)
        outHeight = (std::max)(targetPxHeight, tm.height);
    if (tm.lineCount > 1)
    {
        // ÇEVİRİ METNİ GERÇEKTEN sarılmak zorundaysa: kutuyu tek satırlık
        // orijinal yüksekliğe SIKIŞTIRMAK yerine (bu, sarılan satırların üst
        // üste binmesine yol açardı) DirectWrite'ın kendi hesapladığı DOĞAL
        // (satırlar arasında normal boşluk bırakan) toplam yüksekliği
        // kullanıyoruz.
        outSingleLine = false;
    }
}

HRESULT OverlayEngine::CreateDeviceResources()
{
    // ── D3D11 cihazı ─────────────────────────────────────────────────────────
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, fl, 1, D3D11_SDK_VERSION,
        m_d3dDevice.put(), nullptr, m_d3dCtx.put());
    RETURN_IF_FAILED(hr);

    // ── DXGI swap chain ───────────────────────────────────────────────────────
    ComPtr<IDXGIDevice2>  dxgiDev;
    ComPtr<IDXGIAdapter>  adapter;
    ComPtr<IDXGIFactory2> factory;
    m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice2), dxgiDev.put_void());
    dxgiDev->GetParent(__uuidof(IDXGIAdapter), adapter.put_void());
    adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = m_virtualRect.right  - m_virtualRect.left;
    scd.Height      = m_virtualRect.bottom - m_virtualRect.top;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferCount = 2;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // KRİTİK: eksikti – CreateBitmapFromDxgiSurface
                                                        // D2D1_BITMAP_OPTIONS_TARGET ile bu bayrağı zorunlu
                                                        // kılıyor, yoksa E_INVALIDARG (0x80070057) veriyor.
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.SampleDesc.Count = 1;
    hr = factory->CreateSwapChainForComposition(m_d3dDevice.get(), &scd, nullptr, m_swapChain.put());
    RETURN_IF_FAILED(hr);

    // ── D2D1 ─────────────────────────────────────────────────────────────────
    D2D1_FACTORY_OPTIONS opts{};
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, m_d2dFactory.put());
    RETURN_IF_FAILED(hr);
    hr = m_d2dFactory->CreateDevice(dxgiDev.get(), m_d2dDevice.put());
    RETURN_IF_FAILED(hr);
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dCtx.put());
    RETURN_IF_FAILED(hr);

    // ── DirectWrite ───────────────────────────────────────────────────────────
    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwFactory.put()));
    RETURN_IF_FAILED(hr);
    hr = m_dwFactory->CreateTextFormat(
        m_cfg.fontFamily.c_str(), nullptr,
        m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_cfg.fontSize, L"tr-TR", m_textFormat.put());
    RETURN_IF_FAILED(hr);
    RefreshFontMetricRatios();

    // ── Fırçalar ─────────────────────────────────────────────────────────────
    float tr = GetRValue(m_cfg.textColor) / 255.f;
    float tg = GetGValue(m_cfg.textColor) / 255.f;
    float tb = GetBValue(m_cfg.textColor) / 255.f;
    hr = m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(tr, tg, tb, 1.f), m_textBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: textBrush oluşturulamadı, hr=0x{:08X}", static_cast<unsigned long>(hr));

    float br = GetRValue(m_cfg.bgColor) / 255.f;
    float bg = GetGValue(m_cfg.bgColor) / 255.f;
    float bb = GetBValue(m_cfg.bgColor) / 255.f;
    hr = m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(br, bg, bb, m_cfg.bgOpacity), m_bgBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: bgBrush oluşturulamadı, hr=0x{:08X}", static_cast<unsigned long>(hr));

    // Yazı anahat rengi: metin renginin parlaklığına göre otomatik kontrast
    // (açık renkli yazı -> siyah anahat, koyu renkli yazı -> beyaz anahat)
    float luminance = 0.299f * tr + 0.587f * tg + 0.114f * tb;
    D2D1_COLOR_F outlineColor = (luminance > 0.5f)
        ? D2D1::ColorF(0.f, 0.f, 0.f, 1.f)
        : D2D1::ColorF(1.f, 1.f, 1.f, 1.f);
    hr = m_d2dCtx->CreateSolidColorBrush(outlineColor, m_outlineBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: outlineBrush oluşturulamadı, hr=0x{:08X}", static_cast<unsigned long>(hr));

    Logger::InfoF("OverlayEngine: kaynaklar oluşturuldu (textBrush={}, bgBrush={}, outlineBrush={}).",
        m_textBrush ? "OK" : "NULL", m_bgBrush ? "OK" : "NULL", m_outlineBrush ? "OK" : "NULL");

    // ── DirectComposition ─────────────────────────────────────────────────────
    hr = ::DCompositionCreateDevice(dxgiDev.get(), __uuidof(IDCompositionDevice),
        m_dcompDevice.put_void());
    RETURN_IF_FAILED(hr);
    hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.put());
    RETURN_IF_FAILED(hr);
    hr = m_dcompDevice->CreateVisual(m_dcompVisual.put());
    RETURN_IF_FAILED(hr);
    m_dcompVisual->SetContent(m_swapChain.get());
    m_dcompTarget->SetRoot(m_dcompVisual.get());
    m_dcompDevice->Commit();

    return S_OK;
}

void OverlayEngine::UpdateRegions(std::vector<TranslatedRegion> regions)
{
    std::lock_guard lock(m_regionMutex);
    m_regions = std::move(regions);
    m_dirty   = true;
}

void OverlayEngine::UpdateConfig(const OverlayConfig& newCfg)
{
    // m_regionMutex, render thread'in BeginDraw/DrawRegion/EndDraw sırasında
    // m_d2dCtx/m_dwFactory'yi kullandığı AYNI kilit – bu sayede D2D
    // kaynaklarını burada (çağıran/UI thread'inde) yeniden oluştururken
    // render thread ile çakışma olmaz.
    std::lock_guard lock(m_regionMutex);
    m_cfg = newCfg;

    if (!m_d2dCtx || !m_dwFactory)
        return; // henüz tam başlatılmadı (normalde olmaz, güvenlik kontrolü)

    HRESULT hr = m_dwFactory->CreateTextFormat(
        m_cfg.fontFamily.c_str(), nullptr,
        m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_cfg.fontSize, L"tr-TR", m_textFormat.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: UpdateConfig – textFormat yeniden oluşturulamadı, hr=0x{:08X}",
            static_cast<unsigned long>(hr));
    RefreshFontMetricRatios(); // font ailesi/kalınlığı Ayarlar'dan değişmiş olabilir

    float tr = GetRValue(m_cfg.textColor) / 255.f;
    float tg = GetGValue(m_cfg.textColor) / 255.f;
    float tb = GetBValue(m_cfg.textColor) / 255.f;
    m_textBrush = nullptr;
    hr = m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(tr, tg, tb, 1.f), m_textBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: UpdateConfig – textBrush yeniden oluşturulamadı, hr=0x{:08X}",
            static_cast<unsigned long>(hr));

    float br = GetRValue(m_cfg.bgColor) / 255.f;
    float bg = GetGValue(m_cfg.bgColor) / 255.f;
    float bb = GetBValue(m_cfg.bgColor) / 255.f;
    m_bgBrush = nullptr;
    hr = m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(br, bg, bb, m_cfg.bgOpacity), m_bgBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: UpdateConfig – bgBrush yeniden oluşturulamadı, hr=0x{:08X}",
            static_cast<unsigned long>(hr));

    float luminance = 0.299f * tr + 0.587f * tg + 0.114f * tb;
    D2D1_COLOR_F outlineColor = (luminance > 0.5f)
        ? D2D1::ColorF(0.f, 0.f, 0.f, 1.f)
        : D2D1::ColorF(1.f, 1.f, 1.f, 1.f);
    m_outlineBrush = nullptr;
    hr = m_d2dCtx->CreateSolidColorBrush(outlineColor, m_outlineBrush.put());
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: UpdateConfig – outlineBrush yeniden oluşturulamadı, hr=0x{:08X}",
            static_cast<unsigned long>(hr));

    Logger::Info("OverlayEngine: yapılandırma yeniden başlatma olmadan canlı uygulandı.");
    m_dirty = true; // o an ekranda görünen bölgeler varsa yeni ayarlarla yeniden çizilsin
}

void OverlayEngine::ClearAll()
{
    std::lock_guard lock(m_regionMutex);
    m_regions.clear();
    m_dirty = true;
}

void OverlayEngine::RenderFrame()
{
    ComPtr<IDXGISurface> surface;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void());
    if (FAILED(hr))
    {
        Logger::ErrorF("OverlayEngine: GetBuffer başarısız, hr=0x{:08X}", static_cast<unsigned long>(hr));
        return;
    }

    ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.bitmapOptions         = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    // KRİTİK: dpiX/dpiY sıfır bırakılırsa (D2D1_BITMAP_PROPERTIES1{} varsayılanı)
    // CreateBitmapFromDxgiSurface bazı sistemlerde (özellikle yüksek DPI
    // ölçeklendirmeli 4K monitörlerde) sessizce geçersiz bir hedef üretiyor;
    // bu da tüm çizim komutlarının yutulup EndDraw()'ın D2DERR_WRONG_STATE
    // (0x88990001) döndürmesine yol açıyordu – overlay'in hiç görünmemesinin
    // asıl sebebi buydu.
    bp.dpiX = 96.0f;
    bp.dpiY = 96.0f;

    hr = m_d2dCtx->CreateBitmapFromDxgiSurface(surface.get(), &bp, bitmap.put());
    if (FAILED(hr) || !bitmap)
    {
        Logger::ErrorF("OverlayEngine: CreateBitmapFromDxgiSurface başarısız, hr=0x{:08X}",
            static_cast<unsigned long>(hr));
        return;
    }
    m_d2dCtx->SetTarget(bitmap.get());

    m_d2dCtx->BeginDraw();
    m_d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0)); // Tamamen şeffaf arka plan

    Logger::DebugF("OverlayEngine: RenderFrame – {} bölge çiziliyor (blurMode={}).",
        m_regions.size(), static_cast<int>(m_cfg.blurMode));
    for (auto& region : m_regions)
        DrawRegion(region);

    hr = m_d2dCtx->EndDraw();
    if (FAILED(hr))
    {
        Logger::ErrorF("OverlayEngine: EndDraw başarısız, hr=0x{:08X}", static_cast<unsigned long>(hr));
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED)
            RecreateDevice();
        return;
    }

    hr = m_swapChain->Present(1, 0);
    if (FAILED(hr))
        Logger::ErrorF("OverlayEngine: Present başarısız, hr=0x{:08X}", static_cast<unsigned long>(hr));
}

D2D1_RECT_F OverlayEngine::ComputeScreenRect(const TranslatedRegion& r) const
{
    float x = static_cast<float>(r.bounds.left   - m_virtualRect.left);
    float y = static_cast<float>(r.bounds.top    - m_virtualRect.top);
    float w = static_cast<float>(r.bounds.right  - r.bounds.left);
    float h = static_cast<float>(r.bounds.bottom - r.bounds.top);
    return D2D1::RectF(x, y, x + w, y + h);
}

void OverlayEngine::SetMonitorRects(std::vector<RECT> rects)
{
    m_monitorRects = std::move(rects);
}

float OverlayEngine::ComputeMaxRightForRegion(const TranslatedRegion& r) const
{
    constexpr float kEdgeMargin = 16.f;

    // r.bounds'un SOL-ÜST köşesini içeren fiziksel monitörü bul. Bir OCR
    // kutusu birden fazla monitörle "örtüşemez" (Desktop Duplication her
    // monitörü ayrı yakalar), bu yüzden köşe noktası yeterli.
    for (const RECT& mon : m_monitorRects)
    {
        if (r.bounds.left >= mon.left && r.bounds.left < mon.right &&
            r.bounds.top  >= mon.top  && r.bounds.top  < mon.bottom)
        {
            float monRightLocal = static_cast<float>(mon.right - m_virtualRect.left);
            return monRightLocal - kEdgeMargin;
        }
    }

    // Eşleşme yoksa (monitör listesi henüz ayarlanmadıysa, ya da el ile
    // seçim/one-shot gibi tek bir keyfi bölgede) eski davranışa – tüm sanal
    // masaüstünün sağ kenarına – güvenli biçimde düş.
    float localVirtualWidth = static_cast<float>(m_virtualRect.right - m_virtualRect.left);
    return localVirtualWidth - kEdgeMargin;
}

// Ekranın küçük bir bölgesini canlı BitBlt ile yeniden yakalama denemesi
// (SnapshotScreenRegion) burada BİLEREK yok: overlay penceresi kendi
// kendini WDA_EXCLUDEFROMCAPTURE ile dışladığı için, aynı pencerenin
// üstünden geçen bir BitBlt API'ye göre siyah/boş/tutarsız veri
// döndürebiliyor – bu, çeviri kutusunun ekranda hiç görünmemesine yol
// açan bir hataya sebep oldu. Gerçek "altındaki pikselin ortalaması/
// bulanıklaştırılması" ancak OCR aşamasında (overlay ekrana çizilmeden
// ÖNCE) yakalanan orijinal kare verisi pipeline'dan geçirilirse güvenilir
// biçimde yapılabilir; bu, Capture/OCR/Translation pipeline'ına dokunan
// ayrı bir iyileştirme olarak planlanmalı.

// Bölgenin arka planını çizer. Algılanan arka plan rengi varsa (OCR
// aşamasında, overlay ekrana hiç çizilmeden ÖNCE yakalanan gerçek kare
// verisinden hesaplanmıştı – bkz. OCREngine::EstimateRegionColors) onu
// kullanır, böylece kutu görüntüyle uyumlu/sinerjik olur. Algılama
// başarısız olursa (düşük kontrast, kenar bölgesi vb.) Ayarlar'daki sabit
// renge geri düşer.
static void FillDetectedBackground(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* fallback,
                                    const TranslatedRegion& r, const D2D1_RECT_F& rect, float opacity)
{
    if (r.hasDetectedBgColor)
    {
        // NOT: Bu dolgu bir ara turda BİLEREK sabit %100 opaklığa
        // zorlanmıştı (hayalet/çift yazı görünümü şikâyeti üzerine). Ama bu
        // yanlış teşhisti – kullanıcı düşük opaklığı BİLEREK, boyut/font/
        // anahat farklarını teşhis etmek için kullanıyordu; asıl hatalar
        // (aşırı kalın anahat, griye kayan arka plan tahmini) ayrıca
        // düzeltildi. Kullanıcı isteği üzerine kaydırıcı artık TÜM modlar
        // için (bu dahil) tekrar etkin: opaklık, kullanıcının Ayarlar'da
        // seçtiği değeri (bkz. OverlayConfig::bgOpacity) birebir kullanır.
        ComPtr<ID2D1SolidColorBrush> dynamicBrush;
        float dr = GetRValue(r.detectedBgColor) / 255.f;
        float dg = GetGValue(r.detectedBgColor) / 255.f;
        float db = GetBValue(r.detectedBgColor) / 255.f;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(D2D1::ColorF(dr, dg, db, opacity), dynamicBrush.put())))
        {
            ctx->FillRectangle(rect, dynamicBrush.get());
            return;
        }
    }
    ctx->FillRectangle(rect, fallback);
}

void OverlayEngine::DrawRegion(const TranslatedRegion& r)
{
    // Kutuyu ve font boyutunu BİR KEZ, ortak şekilde hesapla; hem arka plan
    // dolgusu hem de metin çizimi AYNI kutuyu kullansın (bkz.
    // ComputeTextLayoutInfo yorumu – çeviri metni orijinalinden uzun olup
    // sığmazsa ÖNCE kutu yatayda büyütülür, gerçekten ekran kenarına
    // dayanırsa ancak o zaman satırlara sarılıp kutu dikeyde büyütülür).
    D2D1_RECT_F baseRect = ComputeScreenRect(r);

    // Yatayda ne kadar büyüyebileceğimizin sınırı: bölgenin bulunduğu
    // FİZİKSEL MONİTÖRÜN sağ kenarı (bir kenar boşluğuyla) – overlay
    // penceresinin kapladığı TÜM sanal masaüstünün (yani her iki monitörün
    // toplamının) sağ kenarı DEĞİL. Eskiden burada m_virtualRect (tüm
    // monitörlerin birleşimi) kullanılıyordu; bu, monitör 1'in sağ
    // kenarına yakın uzun bir çeviri metninin kutusunun monitör 1'in
    // sınırını fark etmeden monitör 2'nin üzerine doğru büyümesine yol
    // açıyordu (monitör 2 fiziksel olarak farklı bir ekran olsa da, overlay
    // penceresi ikisini birden kapladığı için çizim orada da görünüyordu).
    // Şimdi kutu yalnızca KENDİ monitörünün boş alanına kadar büyüyebilir.
    float maxRight = ComputeMaxRightForRegion(r);

    // ÖZEL DURUM: algılanan bir arka plan rengi varsa (r.hasDetectedBgColor)
    // bu, metnin gri/renkli bir panel, düğme vb. sabit boyutlu bir UI
    // öğesinin üzerinde olduğu anlamına gelir (bkz. OCREngine::EstimateRegionColors).
    // Böyle durumlarda kutuyu orijinal genişliğinin ÖTESİNE yatayda
    // büyütmek, doldurduğumuz rengi o panelin gerçek sınırlarının dışına
    // taşırıp görsel olarak "panelin dışına sızmış" gibi görünmesine yol
    // açıyordu. Bu yüzden algılanan arka planı olan bölgelerde yatay
    // büyümeye HİÇ izin vermiyoruz – availableWidth doğrudan orijinal kutu
    // genişliğine eşitlenir, dolayısıyla sığmayan metin (ComputeTextLayoutInfo
    // içindeki 2. adım) doğrudan alt satıra sarılıp kutu dikeyde büyür,
    // panelin/gri alanın yatay sınırları korunur.
    if (r.hasDetectedBgColor)
        maxRight = baseRect.right;

    float fontSize = m_cfg.fontSize;
    float effectiveWidth  = baseRect.right - baseRect.left;
    float effectiveHeight = 0.f;
    bool  singleLine = true;
    float topInsetRatio = 0.f;
    ComputeTextLayoutInfo(r, baseRect.left, effectiveWidth, maxRight,
        fontSize, effectiveWidth, effectiveHeight, singleLine, topInsetRatio);

    D2D1_RECT_F rect = baseRect;
    rect.right = baseRect.left + effectiveWidth;
    if (effectiveHeight > (baseRect.bottom - baseRect.top))
        rect.bottom = baseRect.top + effectiveHeight;

    // Dolgu için (yalnızca dolgu – metin konumlandırmasını ETKİLEMEZ) küçük
    // bir dışa doğru güvenlik payı ekliyoruz: oyun/uygulama arayüzlerindeki
    // orijinal metnin kendi dış hattı/parlama (glow) efekti, OCR'ın tespit
    // ettiği SIKI mürekkep kutusunun biraz DIŞINA taşabilir. Bu birkaç
    // piksellik pay olmadan, dolgu tam SIKI kutuyla sınırlı kalıp o taşan
    // kenarı örtmüyor, altındaki orijinal metnin ince bir halkası hâlâ
    // görünür kalabiliyordu (bkz. yukarıdaki FillDetectedBackground
    // yorumundaki ana opaklık düzeltmesiyle BİRLİKTE ikinci bir güvenlik
    // katmanı). Payı algılanan yazı boyutuyla orantılı tutuyoruz (küçük
    // yazılarda gereksiz büyük bir dolgu yaratmasın diye), makul bir üst
    // sınırla.
    D2D1_RECT_F fillRect = rect;
    {
        float pad = std::clamp(fontSize * 0.06f, 1.5f, 6.f);
        fillRect.left   -= pad;
        fillRect.top    -= pad;
        fillRect.right  += pad;
        fillRect.bottom += pad;
    }

    switch (m_cfg.blurMode)
    {
    case BlurMode::Blur:
        DrawBlur(r, fillRect);
        break;
    case BlurMode::AverageColor:
        DrawAverageColor(r, fillRect);
        break;
    case BlurMode::SemiTransparent:
    default:
        // Bilinçli olarak sabit/yapılandırılmış renk – "Yarı Saydam" modunun
        // amacı görüntüden bağımsız, öngörülebilir bir görünüm sunmaktır.
        m_d2dCtx->FillRectangle(fillRect, m_bgBrush.get());
        break;
    }

    DrawText(r, rect, fontSize, singleLine, topInsetRatio);
}

// BlurMode::Blur – NOT: Gerçek bir gaussian blur, overlay'in ALTINDAKİ gerçek
// ekran pikseline ihtiyaç duyar. Bu pencere WDA_EXCLUDEFROMCAPTURE ile
// işaretli olduğundan (bkz. Init() – kendi çevirimizin sonsuz döngüye
// girmesini önlemek için ZORUNLU), kendi ekranımızı BitBlt ile CANLI OLARAK
// yeniden yakalamaya çalışmak GÜVENİLMEZDİ (Windows API'ye göre siyah, boş
// veya tutarsız veri döndürüyordu). Bunun yerine artık OCR aşamasında
// (overlay ekrana çizilmeden ÖNCE) yakalanan orijinal piksel verisinden
// hesaplanan GERÇEK arka plan rengini kullanıyoruz – bkz.
// OCREngine::EstimateRegionColors + FillDetectedBackground. Bulanıklaştırma
// efektinin kendisi (gaussian blur) henüz yok, yalnızca renk uyumu var;
// gerçek piksel-düzeyi blur için OCR'da yakalanan küçük görüntü kırpıntısının
// da pipeline'dan geçirilmesi gerekir.
void OverlayEngine::DrawBlur(const TranslatedRegion& r, const D2D1_RECT_F& rect)
{
    // Gerçek gaussian blur (canlı ekran yakalaması) yukarıdaki notta
    // açıklandığı gibi güvenilmez; bunun yerine artık OCR aşamasında
    // yakalanan GERÇEK arka plan rengini kullanıyoruz (bkz. FillDetectedBackground).
    FillDetectedBackground(m_d2dCtx.get(), m_bgBrush.get(), r, rect, m_cfg.bgOpacity);
}

// BlurMode::AverageColor – artık gerçekten "ortalama renk": OCR sırasında
// yakalanan orijinal kare verisinden hesaplanan gerçek arka plan rengini
// kullanır (canlı kendi-kendini-yakalama YOK, dolayısıyla önceki
// WDA_EXCLUDEFROMCAPTURE sorunu da yok).
void OverlayEngine::DrawAverageColor(const TranslatedRegion& r, const D2D1_RECT_F& rect)
{
    FillDetectedBackground(m_d2dCtx.get(), m_bgBrush.get(), r, rect, m_cfg.bgOpacity);
}

void OverlayEngine::DrawText(const TranslatedRegion& r, const D2D1_RECT_F& rect, float fontSize, bool singleLine, float topInsetRatio)
{
    const std::wstring& txt = r.translatedText;
    UINT32 len = static_cast<UINT32>(txt.size());

    // ── Bölgeye özel font/renk: orijinal yazının otomatik algılanan boyutu/
    //    rengi varsa onu kullan (çeviri, orijinaline benzer görünsün);
    //    algılama başarısız olduysa (düşük kontrast vb.) Ayarlar'daki sabit
    //    değerlere geri düş. fontSize, singleLine ve topInsetRatio,
    //    DrawRegion() içinde ComputeTextLayoutInfo ile ZATEN hesaplanıp
    //    geldi – burada tekrar hesaplamıyoruz, böylece arka plan kutusuyla
    //    metin HER ZAMAN aynı kararı kullanır. Bu yalnızca bölge
    //    güncellendiğinde çalışır (60 FPS'lik sürekli bir maliyet DEĞİL),
    //    bu yüzden her seferinde yeni format/fırça oluşturmak sorun teşkil
    //    etmez.
    ComPtr<IDWriteTextFormat> textFormat = m_textFormat;
    if (r.detectedFontSize > 0.f)
    {
        ComPtr<IDWriteTextFormat> custom;
        HRESULT hr = m_dwFactory->CreateTextFormat(
            m_cfg.fontFamily.c_str(), nullptr,
            m_cfg.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"tr-TR", custom.put());
        if (SUCCEEDED(hr))
        {
            if (singleLine)
            {
                // ComputeTextLayoutInfo, kutunun (natural width + güvenlik
                // payı) genişliğinde tek satıra sığdığına ZATEN karar verdi.
                // Buna rağmen ölçüm ile asıl çizim arasında (kerning,
                // yuvarlama vb.) çok ufak bir fark bile DirectWrite'ın
                // gereksiz yere satır atlamasına yol açabiliyordu. Satır
                // atlamayı burada KESİN olarak kapatıyoruz – tek satır
                // olacağı zaten garanti edildiği için bunun bir yan etkisi
                // yok, yalnızca "sınırda" durumlarda yanlışlıkla sarılmayı
                // engelliyor.
                custom->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            textFormat = custom;
        }
        else
            Logger::WarningF("OverlayEngine: hesaplanan font boyutu ({:.1f}px) için format oluşturulamadı.",
                fontSize);
    }

    // KRİTİK (dikey konum kayması / "üstten boşluk"): DirectWrite, metni
    // DOĞAL (varsayılan) satır aralığıyla, layout kutusunun tepesinden
    // fontun kendi ascent payı kadar aşağıda başlatır. Daha önce bu payı
    // fontun GENEL ascent oranıyla (m_fontAscentRatio – aksan/diyakritik
    // gibi en uç durumları da kapsayan, TÜM font için sabit bir değer)
    // hesaplayıp SetLineSpacing(UNIFORM, ...) ile ZORLUYORDUK. Bu YANLIŞTI:
    // örneğin tamamen büyük harfli bir metinde gerçek mürekkep yalnızca
    // cap-height kadardır ve bu, fontun genel ascent oranından belirgin
    // şekilde küçüktür – taban çizgisi olması gerekenden çok daha aşağıya
    // sabitleniyor, bu da metnin kutunun tepesinden gözle görülür şekilde
    // aşağıda başlamasına ("üstten boşluk") ve satır yüksekliğini de
    // zorlamamız nedeniyle boyutun olması gerekenden büyük görünmesine yol
    // açıyordu.
    //
    // Doğru çözüm: DirectWrite'ın satır aralığına HİÇ karışmıyoruz (varsayılanı
    // kullanıyoruz); bunun yerine, bu SATIRDA GEÇEN GERÇEK karakterlere göre
    // ölçülen topInsetRatio (bkz. MeasureInkMetrics) ile mürekkebin doğal
    // layout kutusunun tepesine göre ne kadar aşağıda başlayacağını
    // ÖNCEDEN hesaplayıp, çizim kutusunu bu kadar YUKARI kaydırıyoruz.
    // Sonuç: render edilen mürekkebin GERÇEK üst sınırı, rect.top (OCR'ın
    // tespit ettiği orijinal metnin üst sınırı) ile bire bir örtüşüyor —
    // hem tek satırda hem de (ilk satır için) birden çok satıra sarılan
    // metinlerde aynı şekilde çalışır, bu yüzden singleLine'a göre dallanma
    // gerekmiyor.
    float yShift = fontSize * topInsetRatio;
    D2D1_RECT_F textRect = D2D1::RectF(rect.left, rect.top - yShift, rect.right, rect.bottom);

    ComPtr<ID2D1SolidColorBrush> textBrush    = m_textBrush;
    ComPtr<ID2D1SolidColorBrush> outlineBrush = m_outlineBrush;
    if (r.hasDetectedColor)
    {
        float cr = GetRValue(r.detectedColor) / 255.f;
        float cg = GetGValue(r.detectedColor) / 255.f;
        float cb = GetBValue(r.detectedColor) / 255.f;

        ComPtr<ID2D1SolidColorBrush> customText;
        if (SUCCEEDED(m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(cr, cg, cb, 1.f), customText.put())))
            textBrush = customText;

        // Anahat rengi de algılanan metin rengine göre kontrast hesaplansın.
        float luminance = 0.299f * cr + 0.587f * cg + 0.114f * cb;
        D2D1_COLOR_F oc = (luminance > 0.5f)
            ? D2D1::ColorF(0.f, 0.f, 0.f, 1.f)
            : D2D1::ColorF(1.f, 1.f, 1.f, 1.f);
        ComPtr<ID2D1SolidColorBrush> customOutline;
        if (SUCCEEDED(m_d2dCtx->CreateSolidColorBrush(oc, customOutline.put())))
            outlineBrush = customOutline;
    }

    Logger::DebugF("OverlayEngine: DrawText rect=({:.0f},{:.0f})-({:.0f},{:.0f}) len={} "
                    "algılanan(renk={}, hedefPikselYuksekligi={:.0f}px, hesaplananFontBoyutu={:.1f}px, "
                    "tekSatir={}, ustBoslukOrani={:.3f})",
        rect.left, rect.top, rect.right, rect.bottom, len,
        r.hasDetectedColor ? "OK" : "yok", r.detectedFontSize, fontSize, singleLine ? "evet" : "hayir",
        topInsetRatio);

    if (m_cfg.fontOutline && outlineBrush)
    {
        // Basit ama etkili "sahte anahat": metni 8 yönde kaydırarak anahat
        // rengiyle çizip üzerine asıl metni bindiriyoruz. Glyph geometrisi
        // çıkarmaya gerek kalmadan altyazı tarzı okunur bir kontur elde
        // edilir; maliyeti ihmal edilebilir düzeydedir.
        //
        // HATA DÜZELTMESİ (kullanıcı bildirimi: "harflerin etrafındaki
        // beyaz kaplama aşırı kalın"): kayma miktarı ÖNCEDEN font
        // boyutundan BAĞIMSIZ, SABİT 1.2px idi. Küçük/orta punto çeviri
        // metinlerinde (harf vuruşları zaten ince olduğundan) bu sabit
        // 1.2px kayma, harf kalınlığına kıyasla ORANTISIZ BÜYÜK kalıyor,
        // "kalın beyaz kaplama" görünümü yaratıyordu – büyük punto
        // metinlerde ise tam tersi, neredeyse fark edilmiyordu. Çözüm:
        // kayma miktarını font boyutuyla ORANTILI yapmak (tipik altyazı/
        // caption anahat kalınlığı oranı olan ~%5 civarı), makul bir alt/üst
        // sınırla (çok küçük fontlarda tamamen kaybolmasın, çok büyük
        // fontlarda absürt kalınlaşmasın diye).
        float t = std::clamp(fontSize * 0.05f, 0.5f, 2.2f);
        const D2D1_POINT_2F offsets[8] = {
            { -t, -t }, { 0, -t }, { t, -t },
            { -t,  0 },            { t,  0 },
            { -t,  t }, { 0,  t }, { t,  t },
        };
        for (auto& o : offsets)
        {
            D2D1_RECT_F rr = D2D1::RectF(textRect.left + o.x, textRect.top + o.y,
                                          textRect.right + o.x, textRect.bottom + o.y);
            m_d2dCtx->DrawText(txt.c_str(), len, textFormat.get(), rr, outlineBrush.get());
        }
    }

    m_d2dCtx->DrawText(txt.c_str(), len, textFormat.get(), textRect, textBrush.get());
}

void OverlayEngine::Shutdown()
{
    m_running.store(false);
    if (m_renderThread.joinable()) m_renderThread.join();

    // winrt::com_ptr için serbest bırakma: = nullptr  (.reset() YOKTUR)
    m_dcompVisual  = nullptr;
    m_dcompTarget  = nullptr;
    m_dcompDevice  = nullptr;
    m_outlineBrush = nullptr;
    m_bgBrush      = nullptr;
    m_textBrush    = nullptr;
    m_textFormat   = nullptr;
    m_dwFactory    = nullptr;
    m_d2dCtx       = nullptr;
    m_d2dDevice    = nullptr;
    m_d2dFactory   = nullptr;
    m_swapChain    = nullptr;
    m_d3dCtx       = nullptr;
    m_d3dDevice    = nullptr;

    if (m_hwnd) { ::DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

HRESULT OverlayEngine::RecreateDevice()
{
    Logger::Warning("OverlayEngine: RecreateDevice çağrıldı.");
    Shutdown();
    return Init(m_virtualRect);
}

bool OverlayEngine::ProcessMessages()
{
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) return false;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return true;
}

LRESULT CALLBACK OverlayEngine::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    switch (msg)
    {
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT; // Tüm fare olaylarını geçir
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
