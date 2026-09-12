#pragma once
#include "../Utils/Common.h"
#include "../Utils/Watchdog.h"
#include "../Settings/Config.h"
#include "../Capture/CaptureEngine.h"
#include "../OCR/OCREngine.h"
#include "../Translation/TranslationEngine.h"
#include "../Overlay/OverlayEngine.h"
#include "../Hotkeys/HotkeyManager.h"
#include "../Capture/SelectionWindow.h"
#include "TrayIcon.h"

enum class AppMode { Idle, ContinuousRunning, OneShotWaiting };

class Application
{
public:
    // Çeviri worker thread sayısı. TranslationEngine'in llama.cpp context
    // havuzu boyutuyla BİREBİR eşleşmeli – aksi halde ya worker'lar boşuna
    // bekler (havuz < worker) ya da bazı havuz slotları hiç kullanılmaz
    // (havuz > worker). Tek bir yerden yönetmek için sabit burada tanımlı.
    static constexpr int kTranslationWorkerCount = 2;

    explicit Application(HINSTANCE hInstance);
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    int Run();  // Blocking – returns on exit

private:
    // Initialisation
    HRESULT InitDirectories();
    HRESULT InitComponents();
    void    ShutdownComponents();

    // Mode control
    void StartContinuous();
    void StopContinuous();
    void TriggerOneShot();

    // Ayarlarda "Tam Ekran" seçiliyse (bkz. CaptureAreaMode), tek seferlik ve
    // sürekli modun kullanacağı bölgeyi kullanıcıya fareyle SEÇTİRMEDEN,
    // doğrudan yapılandırılmış monitör(ler)in birleşik sınırlarından
    // hesaplar (InitComponents() içindeki virtualRect hesaplamasıyla aynı
    // mantık – m_capture->GetMonitorRects() zaten cfg.capture.monitorIndices
    // filtresini uygulamış hâlde döner).
    RECT ComputeFullScreenRegion() const;
    // "Aktif Monitör" modu: fare imlecinin o an üzerinde olduğu monitörün
    // sınırlarını döndürür (farklı çözünürlükteki diğer monitörler bu
    // bölgenin tamamen dışında kalır, bkz. Application::OnFrame ->
    // CropFrameToRegion – kesişmeyen kareler baştan elenir).
    RECT ComputeActiveMonitorRegion() const;

    // Tek seferlik çeviri: seçilen bölgeyi GDI ile yakalar (Desktop
    // Duplication'a göre tek-kare senkron yakalama için daha basit).
    CaptureFrame CaptureRegionGDI(const RECT& region) const;

    // Desktop Duplication API ile tek seferlik bölge yakalama. GDI BitBlt
    // (CaptureRegionGDI), DirectX/OpenGL/Vulkan ile render edilen modern oyun
    // içeriğini genelde yakalayamaz (siyah/eski görüntü döner). Bu fonksiyon
    // aynı sürekli-modun kullandığı güvenilir mekanizmayı tek seferlik mod
    // için de kullanır; başarısız olursa CaptureRegionGDI'a geri düşer.
    CaptureFrame CaptureRegionDDA(const RECT& region) const;

    // Sürekli mod: CaptureEngine tüm monitörü yakalar; bu fonksiyon o kareyi
    // yalnızca kullanıcının seçtiği alt-bölgeye (m_continuousRegion) kırpar.
    // Bölge, verilen kareyle hiç örtüşmüyorsa false döner (çoklu monitör).
    bool CropFrameToRegion(const CaptureFrame& src, const RECT& region, CaptureFrame& out) const;

    // Karenin ucuz bir örneklemesini (en fazla 32x32 nokta) alıp basit bir
    // "imza" üretir. Pahalı Windows OCR çağrısından ÖNCE, ekranın son
    // gönderilen kareyle aynı olup olmadığını anlamak için kullanılır –
    // duran/durağan içerikte gereksiz OCR turlarını önler.
    static std::vector<uint8_t> HashFrameSample(const CaptureFrame& frame);
    // Yakalama + OCR + çeviri + overlay + otomatik temizleme – arka plan
    // thread'inde çalışır, mesaj döngüsünü bloklamaz.
    void RunOneShotOnRegion(RECT region);

    // Pipeline (called from capture callback)
    void OnFrame(CaptureFrame frame);
    void ProcessOcrResults(std::vector<OcrRegion> regions);

    // Change detection (compares new regions with previous ones using IoU)
    struct RegionDelta {
        std::vector<OcrRegion> added;
        std::vector<OcrRegion> changed;
        std::vector<uint64_t>  removedIds;
        std::vector<OcrRegion> unchanged;
    };
    RegionDelta DiffRegions(const std::vector<OcrRegion>& oldR,
                            const std::vector<OcrRegion>& newR) const;

    // Hotkey dispatcher
    void OnHotkey(const std::string& id);

    // Tray dispatcher
    void OnTrayCommand(TrayCommand cmd);

    // Ayarlar penceresini açar (modal); "Kaydet"e basılırsa config diske
    // yazılır ve kullanıcıya bazı değişikliklerin yeniden başlatma
    // gerektirebileceği bildirilir (bkz. ApplySettings yorum notu).
    void OpenSettingsWindow();

    // Message window
    static LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);
    HWND CreateMessageWindow();

    // ─── State ──────────────────────────────────────────────────────────────
    HINSTANCE  m_hInstance;
    AppMode    m_mode{ AppMode::Idle };
    RECT       m_continuousRegion{}; // Sürekli mod için kullanıcının seçtiği alan
    std::vector<uint8_t> m_lastFrameHash; // Değişmeyen kareler için pahalı OCR çağrısını atlamaya yarar
    HWND       m_msgHwnd{ nullptr };

    // ─── Paths ──────────────────────────────────────────────────────────────
    fs::path m_appDir;
    fs::path m_logDir;
    fs::path m_cacheDir;
    fs::path m_modelDir;
    fs::path m_configPath;
    fs::path m_translationCachePath;

    // ─── Subsystems ─────────────────────────────────────────────────────────
    std::unique_ptr<ConfigManager>     m_config;
    std::unique_ptr<CaptureEngine>     m_capture;
    std::unique_ptr<OCREngine>         m_ocr;
    std::unique_ptr<TranslationEngine> m_translation;
    std::unique_ptr<OverlayEngine>     m_overlay;
    std::unique_ptr<HotkeyManager>     m_hotkeys;
    std::unique_ptr<TrayIcon>          m_tray;

    // ─── Watchdogs ──────────────────────────────────────────────────────────
    std::unique_ptr<Watchdog> m_captureWatchdog;
    std::unique_ptr<Watchdog> m_ocrWatchdog;

    // ─── Pipeline state ─────────────────────────────────────────────────────
    std::mutex                   m_regionMutex;
    std::vector<OcrRegion>       m_currentRegions;

    // Translation worker pool (2 threads)
    std::vector<std::thread>     m_translationWorkers;
    std::mutex                   m_translateQueueMutex;
    std::condition_variable      m_translateQueueCv;
    std::queue<OcrRegion>        m_translateQueue;
    std::atomic<bool>            m_workersRunning{ false };

    // Overlay region map: id → TranslatedRegion
    std::mutex                   m_overlayMutex;
    std::unordered_map<uint64_t, TranslatedRegion> m_overlayMap;
    // "Nesil sayacı": m_overlayMap'i değiştiren HER işlem (ekleme/silme) bu
    // sayacı bir artırır (bkz. Application.cpp – tüm mutasyon noktalarında
    // m_overlayMutex TUTULURKEN artırılır). FlushOverlayMap(), anlık
    // görüntüyü aldığı andaki sayacı da saklar; render'ı GERÇEKTEN
    // uygulamadan hemen önce, o sırada DAHA YENİ bir nesil oluşup
    // oluşmadığını kontrol eder – oluştuysa kendi (artık BAYAT) anlık
    // görüntüsünü render ETMEZ. Bu, birden fazla bağımsız thread'in (örn.
    // her tek seferlik çeviri bölgesi için ayrı bir "8 saniye sonra sil"
    // thread'i) aynı anda m_overlayMap'i değiştirip FlushOverlayMap
    // çağırdığı durumda, ESKİ bir thread'in – kilidi bırakıp UpdateRegions'ı
    // çağırana kadar OS tarafından geciktirilmiş olması yüzünden – DAHA
    // YENİ bir render'ın ÜZERİNE yazmasını (ve bunun düzeltilecek başka bir
    // tetikleyici olmadığı için overlay'in kalıcı olarak BAYAT bir hâlde
    // "takılı" kalmasını) önler.
    std::atomic<uint64_t>         m_overlayGeneration{ 0 };
    // Her ID için EN SON istenen kaynak metni tutar. Çeviri (llama.cpp)
    // birkaç saniye sürebildiğinden, bir çeviri kuyruğa girdikten SONRA
    // aynı bölgenin metni TEKRAR değişip yeni bir çeviri kuyruğa girebilir.
    // Böyle bir durumda ilk (artık bayat) çevirinin sonucu geldiğinde onu
    // ekrana yazmamak için bu haritayla karşılaştırılıyor.
    std::unordered_map<uint64_t, std::wstring> m_latestRequestedText;

    void StartTranslationWorkers();
    void StopTranslationWorkers();
    void TranslationWorkerLoop();
    // Anlık görüntüyü alıp render'ı ÇAĞIRAN TARAF m_overlayMutex'i TUTUYORKEN
    // yapar (kilitlemez/açmaz) – bkz. Application.cpp'deki yorum: mutasyon
    // (ekleme/silme) ile render çağrısının AYNI kilit altında, ara vermeden
    // yapılması, birden fazla thread'in (örn. tek seferlik çevirilerin
    // bağımsız "N saniye sonra sil" thread'leri) render çağrılarının
    // birbirini YANLIŞ sırada geçmesini YAPISAL OLARAK imkânsız kılar.
    void RenderOverlayMapLocked();
    // Kilitlenmemiş/bağımsız çağrılar için kolaylık sarmalayıcısı: kilidi
    // alır, RenderOverlayMapLocked()'ı çağırır, kilidi bırakır.
    void FlushOverlayMap();

    // OCR rate limiting
    using clock_t = std::chrono::steady_clock;
    std::atomic<clock_t::time_point::rep> m_lastOcrSubmit{};

    // Tek seferlik çeviri devam ederken kapanışı engellemek için sayaç
    // (ShutdownComponents bunu bekler, subsystem'ler erkenden yok edilmesin).
    // NOT: Bu sayaç artık "oturum" değil "kuyruğa girmiş/gösterimde olan
    // BÖLGE" başına sayıyor (bkz. RunOneShotOnRegion + TranslationWorkerLoop) –
    // her bölge çeviri kuyruğuna girince +1, 8sn'lik gösterimi bitip
    // ekrandan kalkınca -1 oluyor.
    std::atomic<int> m_oneShotInFlight{ 0 };

    // Tek seferlik moddan çeviri kuyruğuna giren, çevirisi tamamlanınca
    // TranslationWorkerLoop tarafından 8 saniye sonra otomatik ekrandan
    // kaldırılması gereken bölge ID'leri. Sürekli moddan gelen bölgeler bu
    // sette YOKTUR – onların ömrü OCR diff/kaybolma mantığıyla yönetilir.
    std::mutex                   m_oneShotIdsMutex;
    std::unordered_set<uint64_t> m_oneShotIds;
};
