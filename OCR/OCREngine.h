#pragma once
#include "../Utils/Common.h"
#include "../Settings/Config.h"
#include "../Capture/CaptureEngine.h"

using OcrCallback = std::function<void(std::vector<OcrRegion>)>;

class OCREngine
{
public:
    explicit OCREngine(const OcrConfig& cfg, const std::wstring& srcLang);
    ~OCREngine();

    OCREngine(const OCREngine&)            = delete;
    OCREngine& operator=(const OCREngine&) = delete;

    // Kaynak dil boşsa ("" = Otomatik Algıla) OcrEngine::TryCreateFromUserProfileLanguages()
    // kullanılır; aksi halde belirtilen dil paketiyle OcrEngine oluşturulur.
    HRESULT Init();

    // Ayarlar'dan gelen yeni kaynak dili CANLI olarak uygular (yeniden başlatma
    // gerekmez). Yalnızca OCR motoru yeniden oluşturulur, worker thread'e dokunulmaz.
    HRESULT UpdateSourceLanguage(const std::wstring& newSrcLang);

    // Submit a frame for OCR.  Results arrive via callback (on OCR worker thread).
    // If an identical frame hash is already cached the callback is called with
    // the cached result instead of re-running OCR.
    void SubmitFrame(CaptureFrame frame, OcrCallback cb);

    void Restart();   // Watchdog restart

    // Windows'un "Yazı Tanıma" (OCR) dil paketini kurma sayfasını açar.
    //
    // NOT (revizyon): İlk denemede burada Windows.System.LanguagePackManager
    // WinRT API'siyle GERÇEK tek-tıkla, uygulama-içi bir kurulum yapılmaya
    // çalışıldı. Ama bu tür, KULLANICININ Visual Studio projesinin
    // hedeflediği Windows SDK sürümünde YOKTU (muhtemelen daha yeni bir SDK
    // gerektiriyor) - derleme hatası verdi (C2653/C3861). Riskli bir SDK
    // sürüm varsayımıyla tekrar hataya yol açmamak için, HER Windows 10/11
    // sürümünde ve HER SDK ile GARANTİ ÇALIŞAN yönteme dönüldü: `ms-settings:`
    // URI şemasıyla doğrudan ilgili Windows ayar sayfasını açmak
    // (ShellExecuteW - ekstra bir WinRT tür/SDK bağımlılığı gerektirmez).
    // Bu, kullanıcıyı Ayarlar menüsünde ARAMAKTAN kurtarır (doğrudan doğru
    // sayfayı açar) ama son "Yazı Tanıma'yı ekle" tıklamasını kullanıcının
    // kendisi yapar - tam otomatik değil, ama sıfır ek SDK/derleme riski.
    static void OpenLanguageSettingsPage();

private:
    void   WorkerLoop();
    bool   CheckLanguagePack(const std::wstring& langTag);
    std::vector<OcrRegion> RunOcr(const CaptureFrame& frame);
    bool   PassesQualityFilter(const std::wstring& text) const;
    float  ComputeEnglishWordRatio(const std::wstring& text) const;

    OcrConfig    m_cfg;
    std::wstring m_srcLang; // "" = Otomatik Algıla

    // WinRT OCR engine – m_engineMutex, UpdateSourceLanguage (UI thread) ile
    // WorkerLoop (OCR thread) arasındaki erişimi korur.
    std::mutex                            m_engineMutex;
    winrt::Windows::Media::Ocr::OcrEngine m_ocrEngine{ nullptr };

    // Worker thread
    std::atomic<bool>         m_running{ false };
    std::thread               m_thread;
    std::mutex                m_queueMutex;
    std::condition_variable   m_queueCv;

    struct WorkItem { CaptureFrame frame; OcrCallback cb; };
    std::queue<WorkItem>      m_queue;   // Max 2 items kept (drop old frames)
};
