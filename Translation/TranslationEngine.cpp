#include "../Utils/pch.h"
#include "TranslationEngine.h"
#include "../Utils/Logger.h"
#include <array>
#include <string_view>

// ─── String helpers ──────────────────────────────────────────────────────────
static std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> buf(n);
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), n);
    return std::wstring(buf.begin(), buf.end() - 1);  // exclude null terminator
}

// ─── llama.cpp log yönlendirmesi ─────────────────────────────────────────────
static void LlamaLogCallback(ggml_log_level level, const char* text, void* /*userData*/)
{
    if (!text) return;
    std::string msg(text);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (msg.empty()) return;

    switch (level)
    {
    case GGML_LOG_LEVEL_ERROR: Logger::ErrorF("llama.cpp: {}", msg); break;
    case GGML_LOG_LEVEL_WARN:  Logger::WarningF("llama.cpp: {}", msg); break;
    default: /* INFO/DEBUG seviyelerini bastır */ break;
    }
}

// ─── Dil kodu -> Hy-MT2'nin beklediği tam isim ───────────────────────────────
std::string TranslationEngine::LangCodeToFullName(const std::wstring& code)
{
    static const std::unordered_map<std::wstring, std::string> kMap = {
        { L"tr-TR", "Turkish" },  { L"en-US", "English" },   { L"en-GB", "English" },
        { L"zh-CN", "Chinese" },  { L"fr-FR", "French" },    { L"pt-PT", "Portuguese" },
        { L"es-ES", "Spanish" },  { L"ja-JP", "Japanese" },  { L"ru-RU", "Russian" },
        { L"ar-SA", "Arabic" },   { L"ko-KR", "Korean" },    { L"de-DE", "German" },
        { L"it-IT", "Italian" },  { L"nl-NL", "Dutch" },     { L"pl-PL", "Polish" },
    };
    auto it = kMap.find(code);
    if (it != kMap.end()) return it->second;
    return WideToUtf8(code); // bilinmeyen kod: olduğu gibi geçir
}

TranslationEngine::TranslationEngine(const TranslationConfig& cfg, const fs::path& modelDir)
    : m_cfg(cfg), m_modelDir(modelDir)
{}

TranslationEngine::~TranslationEngine() { Shutdown(); }

HRESULT TranslationEngine::Init()
{
    fs::path modelPath = m_modelDir / m_cfg.ggufFileName;
    if (!fs::exists(modelPath))
    {
        Logger::ErrorF("Translation: GGUF model bulunamadı: {}", modelPath.string());
        return E_FAIL;
    }

    llama_log_set(LlamaLogCallback, nullptr);
    llama_backend_init();

    // KRİTİK: vcpkg'nin ggml derlemesi GGML_BACKEND_DL (dinamik backend
    // yükleme) ile yapılıyor – CUDA backend'i ayrı bir DLL (ggml-cuda.dll)
    // olarak derlenir ama OTOMATİK yüklenmez/kaydedilmez. Bu çağrı
    // yapılmazsa "no backends are loaded" durumuna düşülür ve model
    // sessizce CPU'ya iner (useGpu=true olsa, n_gpu_layers=999 verilse
    // bile) – GPU'nun kullanılmamasının asıl sebebi buydu.
    ggml_backend_load_all();

    // KRİTİK TANI: llama.cpp'nin CPU'nun AVX2/FMA/AVX512 gibi hızlandırma
    // setlerini gerçekten kullanıp kullanmadığını kesin olarak görmek için.
    // Bunlar etkin DEĞİLSE, çıkarım hızı (token/sn) 5-10 kat daha yavaş
    // olabilir – "yavaş çeviri" şikayetlerinin en olası tek nedeni budur.
    Logger::InfoF("llama.cpp sistem bilgisi: {}", llama_print_system_info());

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = m_cfg.useGpu ? 999 : 0; // CUDA derlemesinde tüm katmanları GPU'ya offload eder

    m_model = llama_model_load_from_file(modelPath.string().c_str(), mparams);
    if (!m_model)
    {
        Logger::ErrorF("Translation: model yüklenemedi: {}", modelPath.string());
        llama_backend_free();
        return E_FAIL;
    }
    m_vocab = llama_model_get_vocab(m_model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx   = static_cast<uint32_t>(m_cfg.contextSize);
    cparams.n_batch = std::min<uint32_t>(512, static_cast<uint32_t>(m_cfg.contextSize));
    unsigned hw = std::thread::hardware_concurrency();
    cparams.n_threads       = static_cast<int32_t>(hw > 0 ? hw : 4);
    cparams.n_threads_batch = cparams.n_threads;

    m_ctx = llama_init_from_model(m_model, cparams);
    if (!m_ctx)
    {
        Logger::Error("Translation: llama context oluşturulamadı.");
        llama_model_free(m_model); m_model = nullptr;
        llama_backend_free();
        return E_FAIL;
    }

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    m_sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(m_sampler, llama_sampler_init_top_k(m_cfg.topK));
    llama_sampler_chain_add(m_sampler, llama_sampler_init_top_p(m_cfg.topP, 1));
    llama_sampler_chain_add(m_sampler, llama_sampler_init_temp(m_cfg.temperature));
    llama_sampler_chain_add(m_sampler, llama_sampler_init_penalties(
        /*penalty_last_n=*/64, /*penalty_repeat=*/m_cfg.repetitionPenalty,
        /*penalty_freq=*/0.0f, /*penalty_present=*/0.0f));
    llama_sampler_chain_add(m_sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    Logger::InfoF("TranslationEngine: initialised (llama.cpp / {}).",
                   m_cfg.ggufFileName.empty() ? "Hy-MT2" : WideToUtf8(m_cfg.ggufFileName));
    return S_OK;
}

void TranslationEngine::Shutdown()
{
    if (m_sampler) { llama_sampler_free(m_sampler); m_sampler = nullptr; }
    if (m_ctx)     { llama_free(m_ctx);             m_ctx     = nullptr; }
    if (m_model)   { llama_model_free(m_model);     m_model   = nullptr; }
    m_vocab = nullptr;
    llama_backend_free();
}

void TranslationEngine::RequestCancel()
{
    m_cancelRequested.store(true, std::memory_order_relaxed);
}

HRESULT TranslationEngine::UpdateConfig(const TranslationConfig& newCfg)
{
    bool modelChanged;
    {
        std::unique_lock lock(m_cfgMutex);
        modelChanged = (newCfg.ggufFileName != m_cfg.ggufFileName);
        m_cfg = newCfg;
    }

    if (!modelChanged)
    {
        Logger::Info("TranslationEngine: yapılandırma yeniden başlatma olmadan canlı uygulandı.");
        return S_OK;
    }

    Logger::InfoF("TranslationEngine: model dosyası değişti ({}), yeniden yükleniyor...",
                  WideToUtf8(newCfg.ggufFileName));

    // Devam eden bir Translate() varsa m_llmMutex'i tutuyor olabilir –
    // önce onun bitmesini (ya da RequestCancel ile erken sonlanmasını)
    // bekleyip modeli güvenle kapatıp yeniden yüklüyoruz.
    RequestCancel();
    std::lock_guard lock(m_llmMutex);
    m_cancelRequested.store(false, std::memory_order_relaxed);

    Shutdown();
    HRESULT hr = Init();
    if (FAILED(hr))
        Logger::Error("TranslationEngine: yeni model yüklenemedi.");
    return hr;
}

// ─── Normalise cache key ─────────────────────────────────────────────────────
std::wstring TranslationEngine::Normalise(const std::wstring& text)
{
    std::wstring n = text;
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    auto first = n.find_first_not_of(L" \t\r\n");
    auto last  = n.find_last_not_of (L" \t\r\n");
    return (first == std::wstring::npos) ? L"" : n.substr(first, last - first + 1);
}

// Yalnızca baş/son boşlukları temizler, BÜYÜK/küçük harfi KORUR. Modele
// gönderilecek ASIL metin için kullanılır (bkz. Translate() içindeki yorum) –
// Normalise() ile karıştırılmamalı, o yalnızca ÖNBELLEK anahtarı içindir.
std::wstring TranslationEngine::TrimWhitespace(const std::wstring& text)
{
    auto first = text.find_first_not_of(L" \t\r\n");
    auto last  = text.find_last_not_of (L" \t\r\n");
    return (first == std::wstring::npos) ? L"" : text.substr(first, last - first + 1);
}

// En az bir harf içermesi ve içerdiği TÜM harflerin büyük harf olması
// şartıyla true döner (rakamlar, noktalama, boşluk vb. göz ardı edilir –
// örn. "WI-FI 5G" hâlâ ALL-CAPS sayılır). ::iswupper/::iswlower Windows'un
// geçerli locale'ine göre çalışır; bu da zaten Normalise()'daki ::towlower
// ile tutarlıdır.
bool TranslationEngine::IsAllCaps(const std::wstring& text)
{
    bool hasLetter = false;
    for (wchar_t ch : text)
    {
        if (::iswalpha(ch))
        {
            hasLetter = true;
            if (::iswlower(ch)) return false;
        }
    }
    return hasLetter;
}

// ─── Cache format sürümü ──────────────────────────────────────────────────────
// ÖNEMLİ: Cache diskte KALICI olarak saklanır (bkz. LoadCache/SaveCache) ve
// tek anahtarı normalize edilmiş METİNDİR – hangi PROMPT'un veya hangi
// üretim/uzunluk sınırının o çeviriyi ürettiğinden TAMAMEN BAĞIMSIZDIR.
// Bu, ciddi bir hataya yol açıyordu: BuildPrompt()/RunInference() içindeki
// prompt metni veya maxTokens mantığı iyileştirilse bile (örn. kısa
// çevirilerin gereksiz uzatılmasını önlemek için), DAHA ÖNCE aynı kelime
// için diske yazılmış ESKİ (kötü) sonuç sonsuza kadar geri dönmeye devam
// ediyordu – model ayarları (GPU/CPU, düşük/orta/tam güç vb.) DEĞİŞTİRİLSE
// BİLE, çünkü model HİÇ ÇALIŞTIRILMIYORDU (cache hit). Kullanıcı "aynı
// kelimeleri kaydedip onları mı çağırıyor?" diye sorduğunda haklıydı.
//
// Çözüm: cache dosyasına bir "format sürümü" gömülüyor. LoadCache, dosyadaki
// sürüm bu sabitle eşleşmiyorsa (ya da hiç yoksa – eski format) TÜM cache'i
// sessizce ATAR, böylece o kelimeler bir dahaki sefere GERÇEKTEN yeniden
// çevrilir. Prompt metnini (BuildPrompt) veya üretim/uzunluk mantığını
// (RunInference'daki dynamicCap vb.) ÇEVİRİ KALİTESİNİ etkileyecek şekilde
// her değiştirdiğinde bu sabiti bir artır.
static constexpr int kCacheFormatVersion = 6;
static constexpr const char* kCacheVersionKey = "__cache_format_version";

// ─── Cache ───────────────────────────────────────────────────────────────────
std::wstring TranslationEngine::CacheLookup(const std::wstring& norm) const
{
    std::shared_lock lock(m_cacheMutex);
    auto it = m_cache.find(norm);
    return it == m_cache.end() ? L"" : it->second;
}

void TranslationEngine::CacheStore(const std::wstring& norm, const std::wstring& translated)
{
    std::unique_lock lock(m_cacheMutex);
    m_cache[norm] = translated;
}

void TranslationEngine::ClearCache()
{
    std::unique_lock lock(m_cacheMutex);
    size_t n = m_cache.size();
    m_cache.clear();
    Logger::InfoF("Translation cache: {} girdi elle temizlendi.", n);
}

void TranslationEngine::LoadCache(const fs::path& cacheFile)
{
    if (!fs::exists(cacheFile)) return;
    try {
        std::ifstream f(cacheFile);
        json_t j; f >> j;

        int fileVersion = j.contains(kCacheVersionKey) ? j[kCacheVersionKey].get<int>() : 0;
        if (fileVersion != kCacheFormatVersion)
        {
            Logger::WarningF(
                "Translation cache: format sürümü uyuşmuyor (dosya v{}, beklenen v{}) – "
                "prompt/uzunluk mantığı değiştiği için ESKİ çeviriler GÜVENİLİR değil, "
                "cache'in tamamı atılıyor (bir sonraki çeviriler yeniden üretilecek).",
                fileVersion, kCacheFormatVersion);
            return; // m_cache boş kalır – dosya bir sonraki SaveCache'te yeni sürümle yazılır
        }

        std::unique_lock lock(m_cacheMutex);
        for (auto& [k, v] : j.items())
        {
            if (k == kCacheVersionKey) continue;
            m_cache[Utf8ToWide(k)] = Utf8ToWide(v.get<std::string>());
        }
        Logger::InfoF("Translation cache loaded ({} entries, v{}).", m_cache.size(), fileVersion);
    }
    catch (const std::exception& ex) {
        Logger::ErrorF("Translation cache load failed: {}", ex.what());
    }
}

void TranslationEngine::SaveCache(const fs::path& cacheFile) const
{
    try {
        json_t j;
        j[kCacheVersionKey] = kCacheFormatVersion;
        std::shared_lock lock(m_cacheMutex);
        for (auto& [k, v] : m_cache)
            j[WideToUtf8(k)] = WideToUtf8(v);
        fs::create_directories(cacheFile.parent_path());
        std::ofstream f(cacheFile);
        f << j.dump(2);
        Logger::InfoF("Translation cache saved ({} entries).", m_cache.size());
    }
    catch (const std::exception& ex) {
        Logger::ErrorF("Translation cache save failed: {}", ex.what());
    }
}

// ─── Prompt oluşturma (Hy-MT2 default translation template) ─────────────────

// Kaynak metin (baş/son boşluk temizlendikten sonra) İÇİNDE hiç boşluk/tab
// karakteri kalmıyorsa "tek kelime" sayılır. Bu, "tek kelimeler için tek
// kelimelik karşılık aransın" isteğinin hem prompt'a (aşağıda TEK KELİMEYE
// özel, daha SIKI bir talimat ekleniyor) hem de üretim tavanına (bkz.
// RunInference'daki dynamicCap – tek kelime için çok daha dar bir tavan
// kullanılıyor) yansıtılması için kullanılır. Basit boşluk kontrolü UTF-8
// çok baytlı karakterlerle de güvenlidir (boşluk her zaman tek baytlık
// 0x20/0x09'dur, çok baytlı bir dizinin ORTASINDA asla görünmez).
static bool IsSingleWordUtf8(const std::string& s)
{
    size_t first = s.find_first_not_of(" \t\r\n");
    size_t last  = s.find_last_not_of (" \t\r\n");
    if (first == std::string::npos) return false; // boş metin
    std::string trimmed = s.substr(first, last - first + 1);
    return trimmed.find_first_of(" \t") == std::string::npos;
}

std::string TranslationEngine::BuildPrompt(const std::string& sourceUtf8, const TranslationConfig& cfg) const
{
    std::string targetLang = LangCodeToFullName(cfg.dstLang);
    bool singleWord = IsSingleWordUtf8(sourceUtf8);

    std::string userMsg;
    if (singleWord)
    {
        // Tek kelimeye özel talimat – ÖNEMLİ NOT: bu talimat DAHA ÖNCE çok
        // uzun/detaylıydı (birkaç cümlelik açıklama + somut örnek). Bu,
        // küçük (1.8B) modelde YENİ ve CİDDİ bir sorna yol açtı: model,
        // özellikle anlamsız/belirsiz kısa girdilerde ("ego;" gibi yanlış
        // okunmuş bir simge, ya da "HOME" gibi bağlamsız tek kelime)
        // ÇEVİRİ ÜRETMEK YERİNE kendi TALİMAT METNİMİZİ (bazen Türkçeye
        // çevrilmiş hâliyle) GERİ TÜKÜRMEYE başladı – örn. gözlemlenen
        // gerçek bir örnek: 'ego;' -> 'Aşağıdaki kaynak metni, bir cümle
        // değil, tek...' (tam olarak bu talimatın kendisinin bir parçası).
        // Uzun talimat, modelin "girdi" ile "talimat" arasındaki sınırı
        // bulanıklaştırıyordu. Çözüm: talimatı KISA tutmak.
        //
        // EK SORUN (somut örnek: "LOGIN" -> "GİRİŞ YAPMAK", "REGISTER" ->
        // "KAYIT ONAMA"): "-ing" kuralı bunları YAKALAMIYORDU çünkü bu
        // kelimeler "-ing" ile bitmiyor. Model, bir EYLEM/KOMUT kelimesi
        // (bir buton: Login, Save, Register, Submit...) gördüğünde hedef
        // dilde EYLEMİ TANIMLAYAN bir MASTAR/ULAÇ formuna kayıyordu (Türkçe
        // özelinde "-mak/-mek" ile biten mastar: "yapmak", "olmak" gibi) –
        // oysa gerçek arayüzlerde bu tür butonlar her zaman KISA, EMİR
        // KİPİNDE bir komut olarak yazılır (Türkçe'de "Giriş Yap", "Kaydet",
        // "Kayıt Ol" gibi – "Giriş Yapmak", "Kaydetmek" DEĞİL). Bunu da tek
        // bir kısa cümleyle, somut örnekle kapatıyoruz.
        userMsg =
            "Translate this single UI word/term into " + targetLang +
            " using the short noun/term a native " + targetLang +
            " software UI would use (not a verb phrase, even for English "
            "\"-ing\" words - e.g. \"Gaming\" -> \"Oyun\", not \"oyun "
            "oynamak\"). If it is a command/button word (e.g. \"Login\", "
            "\"Save\", \"Register\"), use the short IMPERATIVE command form "
            "real UIs use in " + targetLang + ", never a descriptive "
            "infinitive - e.g. in Turkish \"Login\" -> \"Giriş Yap\", NOT "
            "\"Giriş Yapmak\". Output ONLY the translated word/term, nothing "
            "else:\n\n" + sourceUtf8;
    }
    else
    {
        userMsg =
            "Translate the following text into " + targetLang +
            ". Output ONLY the translated text, with no additional words, "
            "explanations, or content that is not present in the source text. "
            "Do not add sentences, clarifications, or context that the source "
            "does not contain. Preserve the original meaning and length as "
            "closely as possible: a single word must be translated as a single "
            "word, and a short UI button/label must stay a short UI button/"
            "label of comparable length in " + targetLang + ", never a full "
            "sentence:\n\n" + sourceUtf8;
    }

    llama_chat_message msg{ "user", userMsg.c_str() };

    const char* tmpl = llama_model_chat_template(m_model, /*name=*/nullptr);

    std::vector<char> buf(userMsg.size() + 1024);
    int32_t n = llama_chat_apply_template(tmpl, &msg, 1, /*add_ass=*/true,
                                           buf.data(), static_cast<int32_t>(buf.size()));
    if (n < 0)
    {
        Logger::Error("Translation: chat template uygulanamadı.");
        return {};
    }
    if (static_cast<size_t>(n) > buf.size())
    {
        buf.resize(static_cast<size_t>(n));
        n = llama_chat_apply_template(tmpl, &msg, 1, true,
                                       buf.data(), static_cast<int32_t>(buf.size()));
    }
    return std::string(buf.data(), static_cast<size_t>(n));
}

// ─── llama.cpp ile üretim ────────────────────────────────────────────────────
std::string TranslationEngine::RunInference(const std::string& sourceUtf8, const TranslationConfig& cfg)
{
    std::string prompt = BuildPrompt(sourceUtf8, cfg);
    if (prompt.empty())
        return {};

    // Yeni istek başlıyor: önceki (varsa) iptal bayrağını sıfırla.
    m_cancelRequested.store(false, std::memory_order_relaxed);

    // Her çeviri bağımsız bir istektir: KV-cache'in MANTIKSAL konumunu
    // (sequence pozisyonunu) sıfırla. data=false kullanıyoruz – true
    // vermek, context'in TÜM KV-cache belleğini (n_ctx kadar) fiziksel
    // olarak sıfırlardı; bu gereksiz bir maliyettir çünkü zaten yeni
    // token'lar eski verilerin üzerine yazılacak (okunmadan önce).
    llama_memory_clear(llama_get_memory(m_ctx), /*data=*/false);
    llama_sampler_reset(m_sampler);

    // Tokenise
    int32_t nMax = static_cast<int32_t>(prompt.size()) + 32;
    std::vector<llama_token> tokens(static_cast<size_t>(nMax));
    int32_t n = llama_tokenize(m_vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                tokens.data(), static_cast<int32_t>(tokens.size()),
                                /*add_special=*/true, /*parse_special=*/true);
    if (n < 0)
    {
        tokens.resize(static_cast<size_t>(-n));
        n = llama_tokenize(m_vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                            tokens.data(), static_cast<int32_t>(tokens.size()), true, true);
    }
    tokens.resize(static_cast<size_t>(n));

    Logger::DebugF("Translate: prompt {} token'a ayrıldı", tokens.size());

    if (static_cast<int>(tokens.size()) >= cfg.contextSize)
    {
        Logger::WarningF("Translate: prompt ({} token) context boyutunu ({}) aşıyor, kırpılıyor.",
                          tokens.size(), cfg.contextSize);
        tokens.resize(static_cast<size_t>(cfg.contextSize) - 1);
    }

    llama_batch batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    if (llama_decode(m_ctx, batch) != 0)
    {
        Logger::Error("Translate: prompt decode başarısız.");
        return {};
    }

    // KRİTİK: cfg.maxTokens (varsayılan 512) sabit bir tavan – kısa bir
    // kaynak metin (tek kelime, kısa bir UI etiketi) için modelin
    // "uzayabileceği" fiziksel alan bu kadar geniş olunca, küçük (1.8B)
    // model bazen bunu bir davet gibi algılayıp gereksiz yere uzun/
    // halüsinasyonlu bir çıktı üretebiliyor (bkz. Translate() içindeki
    // "KNOW MORE" yorumu – prompt düzeltmesi tek başına bunu tamamen
    // önlemiyor). Bunu YAPISAL olarak imkânsız kılmak için, üretime izin
    // verilen token sayısını KAYNAK METNİN token uzunluğuyla orantılı bir
    // tavana da bağlıyoruz: kısa girdi -> kısa çıktı fiziksel olarak
    // zorunlu hâle gelir.
    //
    // TEK KELİME için ayrıca çok daha SIKI bir tavan kullanıyoruz (çarpan x3,
    // taban 8, mutlak tavan 24 token) – "tek kelimeler için tek kelimelik
    // karşılık aransın" isteği burada YAPISAL olarak da destekleniyor:
    // BuildPrompt'taki talimat modele "tek kelime/terim" demesine rağmen
    // yine de birkaç kelimelik bir ifadeye kayarsa, en azından TAM BİR
    // CÜMLEYE dönüşmesi fiziksel olarak imkânsız hâle geliyor. Mutlak
    // tavan 24, en uzun tek "kelime"lerin bile (örn. Almanca birleşik
    // sözcükler ya da tek kelimelik ama çok heceli Türkçe karşılıklar)
    // birkaç token'a bölünebilmesine yetecek kadar cömert tutuldu.
    //
    // Çok kelimeli (cümle) girdi için çarpan (x6) ve taban (16), Latince
    // olmayan hedef dillerde (örn. Türkçe/Rusça) kaynaktan daha fazla token
    // gerekebilmesi ihtimaline karşı bilerek cömert tutuldu; amaç "makul"
    // bir üst sınır koymak, çeviriyi agresifçe kısıtlamak değil.
    std::string srcOnly = sourceUtf8; // yalnızca kaynak metnin token sayısı için, prompt şablonu HARİÇ
    std::vector<llama_token> srcTokens(srcOnly.size() + 8);
    int32_t nSrc = llama_tokenize(m_vocab, srcOnly.c_str(), static_cast<int32_t>(srcOnly.size()),
                                   srcTokens.data(), static_cast<int32_t>(srcTokens.size()),
                                   /*add_special=*/false, /*parse_special=*/false);
    int32_t srcTokenCount = (nSrc >= 0) ? nSrc : static_cast<int32_t>(srcOnly.size() / 2 + 1);

    bool singleWord = IsSingleWordUtf8(srcOnly);
    int32_t dynamicCap = singleWord
        ? std::min(24, std::max(8, srcTokenCount * 3 + 8))
        : std::max(16, srcTokenCount * 6 + 16);
    int32_t effectiveMaxTokens = std::min(cfg.maxTokens, dynamicCap);
    Logger::DebugF("Translate: kaynak {} token ({}), dinamik üretim tavanı {} (config tavanı {}).",
                   srcTokenCount, singleWord ? "tek kelime" : "çok kelimeli", effectiveMaxTokens, cfg.maxTokens);

    std::string out;
    out.reserve(256);
    for (int step = 0; step < effectiveMaxTokens; ++step)
    {
        // Uygulama sürekli modu durdurup çeviri worker'larını iptal etmek
        // istediğinde (RequestCancel), üretimi maxTokens'a kadar beklemek
        // yerine burada erken çıkıyoruz – aksi halde arayüz uzun bir
        // süre "kilitlenmiş" gibi görünebilir.
        if (m_cancelRequested.load(std::memory_order_relaxed))
        {
            Logger::Debug("Translate: iptal istendi, üretim erken durduruldu.");
            break;
        }

        llama_token newTok = llama_sampler_sample(m_sampler, m_ctx, -1);
        llama_sampler_accept(m_sampler, newTok);

        if (llama_vocab_is_eog(m_vocab, newTok))
            break;

        char piece[256];
        int32_t pn = llama_token_to_piece(m_vocab, newTok, piece, sizeof(piece),
                                           /*lstrip=*/0, /*special=*/false);
        if (pn > 0) out.append(piece, static_cast<size_t>(pn));

        llama_batch nextBatch = llama_batch_get_one(&newTok, 1);
        if (llama_decode(m_ctx, nextBatch) != 0)
        {
            Logger::WarningF("Translate: adım {} decode başarısız, üretim durduruldu.", step);
            break;
        }
    }

    Logger::DebugF("Translate: {} karakter üretildi.", out.size());
    return out;
}

// ─── Translate (public API) ──────────────────────────────────────────────────
std::wstring TranslationEngine::Translate(const std::wstring& input)
{
    // ÖNEMLİ HATA DÜZELTMESİ: Normalise() metni KÜÇÜK HARFE çevirir – bu
    // yalnızca ÖNBELLEK ANAHTARI için doğru bir davranıştır ("Rewards" ve
    // "REWARDS" aynı çeviriyi paylaşmalı). Önceden bu küçük-harfli hâl
    // (norm) YANLIŞLIKLA modele gönderilen ASIL METİN olarak da
    // kullanılıyordu. Bu, gözlemlenen iki tuhaflığın da GERÇEK nedeniydi:
    //   1) "REWARDS" -> "ÖDüLLER" gibi tutarsız/karışık büyük-küçük harfli
    //      çıktılar – model "rewards" (tamamen küçük harf) aldığı için
    //      BÜYÜK HARF sinyalini tamamen kaybediyordu.
    //   2) "KNOW MORE" gibi kısa bir İngilizce UI etiketinin "Daha fazla
    //      bilgi edinmek istiyorsanız…" gibi anlamsızca uzun/halüsinasyonlu
    //      bir cümleye dönüşmesi – küçük (1.8B) bir modelde, orijinal
    //      BÜYÜK HARFLİ bir düğme/etiket metni yerine sıradan küçük harfli
    //      bir ifade görmek, modelin bunu daha "konuşma tarzı" bir girdi
    //      sanıp gereksiz yere genişletmesine katkıda bulunuyor olabilir.
    // Çözüm: ÖNBELLEK anahtarı için hâlâ normalize edilmiş metni kullan,
    // ama MODELE gönderilen metin orijinal büyük/küçük harfini KORUSUN.
    std::wstring norm = Normalise(input);
    if (norm.empty()) return {};

    // Orijinal metin TAMAMEN büyük harfliyse, çıktıyı da büyük harfe
    // zorlayacağız (aşağıda hem cache-hit hem yeni üretim yolunda tek bir
    // noktadan uygulanıyor). ÖNEMLİ: bu kontrol INPUT (mevcut çağrının
    // orijinal metni) üzerinde yapılıyor, cache'teki değer üzerinde DEĞİL –
    // çünkü aynı normalize anahtar hem "Rewards" hem "REWARDS" için aynı
    // cache girdisini paylaşabilir; ALL-CAPS zorlaması her çağrıda O ANKİ
    // orijinal metnin kendi büyük/küçük harf durumuna göre yapılmalı.
    bool forceUpper = IsAllCaps(input);

    // m_cfg'nin thread-safe bir anlık görüntüsünü BURADA (cache-hit yolu
    // dahil her durumda kullanılabilmesi için) alıyoruz.
    TranslationConfig cfgSnapshot;
    { std::shared_lock lock(m_cfgMutex); cfgSnapshot = m_cfg; }

    // HATA DÜZELTMESİ (Türkçe İ/I karışıklığı): ::CharUpperBuffW,
    // ÇAĞRAN THREAD'İN GEÇERLİ LOCALE'İNİ kullanır – bu genelde Türkçe
    // DEĞİLDİR (örn. "invariant" ya da "en-US" olabilir). Sonuç: Latince
    // 'i' harfi Türkçe'deki gibi noktalı büyük harfe ('İ') değil, düz 'I'
    // harfine dönüşüyordu (örn. "harika" -> "HARIKA" olması gerekirken
    // "HARIKA" değil "HARIKA" – asıl fark noktasız 'ı' harfinde daha
    // belirgindi: "başlayın" -> "BAŞLAYıN" gibi YARI büyütülmüş, karışık
    // sonuçlar üretiyordu, çünkü 'ı' harfi hiç büyütülmüyordu). Çözüm:
    // hedef dilin LOCALE ADINI (cfgSnapshot.dstLang, örn. L"tr-TR")
    // ::LCMapStringEx'e vererek DİL-DUYARLI büyütme yapmak.
    auto ApplyCase = [forceUpper, dstLang = cfgSnapshot.dstLang](std::wstring s) -> std::wstring
    {
        if (!forceUpper || s.empty()) return s;

        std::wstring out(s.size(), L'\0');
        int n = ::LCMapStringEx(
            dstLang.empty() ? LOCALE_NAME_USER_DEFAULT : dstLang.c_str(),
            LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING,
            s.c_str(), static_cast<int>(s.size()),
            out.data(), static_cast<int>(out.size()),
            nullptr, nullptr, 0);
        if (n > 0)
        {
            out.resize(static_cast<size_t>(n));
            return out;
        }
        // LCMapStringEx başarısız olduysa (örn. geçersiz/desteklenmeyen
        // locale adı) genel (locale-duyarsız) yönteme güvenli şekilde düş.
        ::CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
        return s;
    };

    std::wstring cached = CacheLookup(norm);
    if (!cached.empty()) return ApplyCase(cached);

    std::wstring trimmedOriginal = TrimWhitespace(input); // büyük/küçük harf KORUNARAK, yalnızca baş/son boşluk temizlenir
    std::string utf8 = WideToUtf8(trimmedOriginal);
    std::string resultUtf8;

    try
    {
        std::lock_guard lock(m_llmMutex);
        resultUtf8 = RunInference(utf8, cfgSnapshot);
    }
    catch (const std::exception& ex)
    {
        Logger::ErrorF("Translation: llama.cpp inference hatası: {}", ex.what());
        return {};
    }

    // Baş/son boşlukları temizle (model bazen satır başında boşluk üretir)
    auto first = resultUtf8.find_first_not_of(" \t\r\n");
    auto last  = resultUtf8.find_last_not_of (" \t\r\n");
    if (first == std::string::npos)
        resultUtf8.clear();
    else
        resultUtf8 = resultUtf8.substr(first, last - first + 1);

    std::wstring wresult = Utf8ToWide(resultUtf8);

    // HATA KORUMASI (prompt sızıntısı): küçük (1.8B) model, özellikle
    // anlamsız/belirsiz tek-kelimelik OCR girdilerinde ("ego;" gibi yanlış
    // okunmuş bir simge, ya da "HOME" gibi kısa bir kelime) BAZEN çeviri
    // ÜRETMEK YERİNE kendi SİSTEM PROMPT'UMUZUN bir kısmını (Türkçeye
    // çevrilmiş hâliyle bile) GERİ TÜKÜRÜYOR – örn. gözlemlenen gerçek bir
    // örnek: 'ego;' -> 'Aşağıdaki kaynak metni, bir cümle değil, tek'.
    // Bu, prompt'ta kullanılan birkaç ayırt edici Türkçe ifadeyi ARAMAK
    // suretiyle tespit edilip YAKALANABİLİR: gerçek bir çeviri bu spesifik
    // kelime kombinasyonlarını (bilhassa "kaynak metni", "aşağıdaki
    // kaynak") ASLA içermez. Yakalanırsa, kullanıcıya anlamsız/utanç verici
    // bir "çeviri" göstermek yerine GÜVENLİ bir şekilde ORİJİNAL metni
    // aynen döndürüyoruz (çevrilmemiş göstermek, yanlış/saçma bir cümle
    // göstermekten HER ZAMAN daha iyidir).
    static const std::array<std::wstring_view, 4> kPromptLeakMarkers = {
        L"kaynak metni", L"kaynak metin", L"aşağıdaki kaynak", L"tek kelime"
    };
    bool looksLikePromptLeak = false;
    {
        std::wstring lowered = wresult;
        ::CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
        for (auto marker : kPromptLeakMarkers)
        {
            if (lowered.find(marker) != std::wstring::npos) { looksLikePromptLeak = true; break; }
        }
    }
    if (looksLikePromptLeak)
    {
        Logger::WarningF("Translate: '{}' için model prompt sızıntısı üretti ('{}'), orijinal metin korunuyor.",
                          WideToUtf8(trimmedOriginal), WideToUtf8(wresult));
        wresult = trimmedOriginal; // çevrilmemiş ama en azından ANLAMLI bir sonuç
    }

    if (!wresult.empty())
        CacheStore(norm, wresult); // cache'e HAM (büyük harfe zorlanmamış) hâli koy
    return ApplyCase(wresult);
}
