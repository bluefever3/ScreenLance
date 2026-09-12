#pragma once
#include "../Utils/Common.h"
#include "../Settings/Config.h"

// Thread-safe translation engine.
// Uses Tencent Hy-MT2-1.8B (GGUF, Q8_0 quantisation) via llama.cpp for
// fully offline, local LLM-based translation. Replaces the previous
// Marian ONNX encoder/decoder pipeline (model too old to compile against
// current ONNX Runtime).
//
// NOT (paralel context havuzu denemesi GERİ ALINDI): Daha önce burada
// birden fazla bağımsız llama_context ("havuz") tutan bir tasarım vardı.
// Pratikte bu, thread sayısını havuz boyutuna bölmek zorunda bıraktığı
// için (CPU oversubscription'ı önlemek adına) HER TEK çevirinin belirgin
// şekilde YAVAŞLAMASINA yol açtı (~1-2sn'den 10-17sn'ye) – çünkü art arda
// gelen istekler pratikte nadiren gerçekten aynı anda çalışıyordu, ama her
// biri sürekli yarı hızda (yarı thread ile) çalışıyordu. Net etki bir
// kazanç değil kayıptı. Bu yüzden kanıtlanmış çalışan TEK context + mutex
// tasarımına geri dönüldü.
class TranslationEngine
{
public:
    explicit TranslationEngine(const TranslationConfig& cfg,
                               const fs::path& modelDir);
    ~TranslationEngine();

    TranslationEngine(const TranslationEngine&)            = delete;
    TranslationEngine& operator=(const TranslationEngine&) = delete;

    HRESULT Init();
    void    Shutdown();

    // Translates text.  Returns translated string or empty on failure.
    // Thread-safe: may be called from multiple workers (serialised internally,
    // since a single llama_context can only run one decode at a time).
    std::wstring Translate(const std::wstring& input);

    // Devam eden (varsa) üretimi olabildiğince ÇABUK durdurmak için atomik
    // bir bayrak ayarlar – jenerasyon döngüsü her adımda bunu kontrol eder.
    // Uygulama sürekli modu durdururken çeviri worker thread'lerini
    // join() etmeden ÖNCE bunu çağırmalı, aksi halde uzun bir üretimin
    // (maxTokens'a kadar) bitmesini beklemek arayüzü kilitleyebilir.
    void RequestCancel();

    // Translation cache (session-persistent, keyed on normalised source text)
    std::wstring CacheLookup(const std::wstring& normalised) const;
    void         CacheStore (const std::wstring& normalised, const std::wstring& translated);
    void         LoadCache  (const fs::path& cacheFile);
    void         SaveCache  (const fs::path& cacheFile) const;
    // Bellekteki çeviri önbelleğini tamamen boşaltır (diskteki dosyayı
    // silmez – bir sonraki SaveCache çağrısı, örn. uygulama kapanırken,
    // dosyayı zaten boş/güncel içerikle yeniden yazacaktır). Kullanıcının
    // Ayarlar'daki "Önbelleği Temizle" düğmesinden çağrılır.
    void         ClearCache();

    // Ayarlar penceresinden gelen yeni yapılandırmayı CANLI olarak uygular.
    // Hedef dil, örnekleme parametreleri (sıcaklık/topP/topK) ve maxTokens
    // yalnızca üretim sırasında okunduğu için model yeniden yüklenmez.
    // YALNIZCA ggufFileName değiştiyse (farklı model dosyası seçildiyse)
    // mevcut model kapatılıp yenisi yüklenir (birkaç saniye sürebilir).
    HRESULT UpdateConfig(const TranslationConfig& newCfg);

private:
    // Hy-MT2 prompt oluşturma (modelin gömülü chat template'i üzerinden).
    // cfg parametre olarak alınır (m_cfg'yi doğrudan OKUMAZ) – Translate()
    // her çağrıda m_cfg'nin THREAD-SAFE bir anlık görüntüsünü (snapshot)
    // alıp buraya geçirir; aksi halde çeviri thread'i m_cfg'yi okurken
    // UpdateConfig() aynı anda yazabilir (veri yarışı/UB).
    std::string BuildPrompt(const std::string& sourceUtf8, const TranslationConfig& cfg) const;

    // llama.cpp ile tek seferlik üretim (prompt -> çeviri metni)
    std::string RunInference(const std::string& sourceUtf8, const TranslationConfig& cfg);

    // ISO dil kodunu (örn. "tr-TR") Hy-MT2'nin beklediği tam isme çevirir (örn. "Turkish")
    static std::string LangCodeToFullName(const std::wstring& code);

    TranslationConfig  m_cfg;
    mutable std::shared_mutex m_cfgMutex; // m_cfg'ye paralel okuma/UpdateConfig yazma erişimini korur
    fs::path           m_modelDir;

    // llama.cpp
    llama_model*        m_model  { nullptr };
    llama_context*      m_ctx    { nullptr };
    llama_sampler*      m_sampler{ nullptr };
    const llama_vocab*  m_vocab  { nullptr };
    mutable std::mutex  m_llmMutex; // llama_context tek seferde tek decode'a izin verir

    std::atomic<bool> m_cancelRequested{ false };

    // Translation cache
    mutable std::shared_mutex                        m_cacheMutex;
    mutable std::unordered_map<std::wstring, std::wstring> m_cache;

    static std::wstring Normalise(const std::wstring& text);
    // Yalnızca baş/son boşluk temizler, büyük/küçük harfi KORUR (bkz. .cpp yorumu).
    static std::wstring TrimWhitespace(const std::wstring& text);
    // Bkz. TranslationEngine.cpp'deki yorum: orijinal metin TAMAMEN büyük
    // harfliyse (rakam/noktalama/boşluk göz ardı edilir, en az bir harf
    // şartıyla) true döner. Bunu tespit etmek, ALL-CAPS bir düğme/etiketin
    // çevirisinin küçük modelin doğal eğilimiyle yalnızca ilk harfi büyük
    // bir hâle dönüşmesini (örn. "REWARDS" -> "Ödüller") sonradan
    // düzeltebilmek için kullanılır.
    static bool IsAllCaps(const std::wstring& text);
};
