#include "../Utils/pch.h"
#include "Application.h"
#include "../Utils/Logger.h"
#include "../Settings/SettingsWindow.h"

// ─── Log yardımcısı: wstring'i doğru UTF-8'e çevirir ────────────────────────
// (Türkçe karakterler içeren çeviri metinlerini loga okunabilir yazmak için.
//  Naif wchar_t->char daraltması Türkçe karakterleri bozar.)
static std::string NarrowForLog(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ─── Constructor / Destructor ────────────────────────────────────────────────
Application::Application(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
    // Determine base directory next to the .exe
    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    m_appDir              = fs::path(exePath).parent_path();
    m_logDir              = m_appDir / "Logs";
    m_cacheDir            = m_appDir / "Cache";
    m_modelDir            = m_appDir / "Models";
    m_configPath          = m_appDir / "Config" / "settings.json";
    m_translationCachePath= m_cacheDir / "translation_cache.json";
}

Application::~Application()
{
    ShutdownComponents();
}

// ─── Run ─────────────────────────────────────────────────────────────────────
int Application::Run()
{
    Logger::Init(m_logDir);
    Logger::Info("=== ScreenLance starting ===");

    if (FAILED(InitDirectories())) return 1;
    if (FAILED(InitComponents()))  return 1;

    Logger::Info("Ready. Waiting for commands.");

    MSG msg{};
    while (true)
    {
        // Process Windows messages
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (!OverlayEngine::ProcessMessages()) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(8)); // ~120 Hz idle
    }
done:
    Logger::Info("=== ScreenLance exiting ===");
    ShutdownComponents();
    Logger::Shutdown();
    return static_cast<int>(msg.wParam);
}

// ─── Init ────────────────────────────────────────────────────────────────────
HRESULT Application::InitDirectories()
{
    std::error_code ec;
    for (auto& d : { m_logDir, m_cacheDir, m_modelDir, m_configPath.parent_path() })
        fs::create_directories(d, ec);
    return S_OK;
}

HRESULT Application::InitComponents()
{
    // Config
    m_config = std::make_unique<ConfigManager>(m_configPath);
    m_config->Load();
    auto& cfg = m_config->Get();

    // Message-only window (for hotkeys + tray)
    m_msgHwnd = CreateMessageWindow();
    if (!m_msgHwnd) return E_FAIL;

    // Tray icon
    m_tray = std::make_unique<TrayIcon>(m_msgHwnd, m_hInstance);
    m_tray->SetCallback([this](TrayCommand c){ OnTrayCommand(c); });
    m_tray->Create(L"ScreenLance – Çevrimdışı Ekran Çevirisi");

    // Hotkeys
    m_hotkeys = std::make_unique<HotkeyManager>(m_msgHwnd);
    m_hotkeys->SetHandler([this](const std::string& id){ OnHotkey(id); });
    m_hotkeys->RegisterAll(cfg.hotkeys);

    // OCR engine
    m_ocr = std::make_unique<OCREngine>(cfg.ocr, cfg.translation.srcLang);
    if (FAILED(m_ocr->Init()))
    {
        Logger::Error("OCR init failed – cannot start.");

        // KULLANICI DENEYİMİ İYİLEŞTİRMESİ: eskiden burada yalnızca "en-US
        // dil paketini yükleyin" diyen, kullanıcıyı Ayarlar menüsünde manuel
        // aramaya yönlendiren pasif bir hata mesajı vardı. Artık
        // OCREngine::OpenLanguageSettingsPage() ile kullanıcıyı DOĞRUDAN
        // doğru Windows ayar sayfasına götürebiliyoruz (aramasına gerek
        // kalmıyor) - tam otomatik tek-tık kurulum değil (bkz. OCREngine.h
        // yorumu: daha yeni bir WinRT API/SDK sürümü gerektiriyordu, derleme
        // hatası verdiği için HER SDK'da çalışan bu daha basit yönteme
        // dönüldü), ama yine de kullanıcının işini büyük ölçüde kolaylaştırıyor.
        int choice = ::MessageBoxW(nullptr,
            L"OCR başlatılamadı – Windows'un \"Yazı Tanıma\" dil paketi "
            L"(en-US) eksik olabilir.\n\n"
            L"Şimdi ilgili Windows ayar sayfasını açmak ister misiniz? "
            L"(Açılan sayfada dili seçip \"Seçenekler > Yazı Tanıma'yı ekle\" "
            L"demeniz yeterli; işlem bitince ScreenLance'i yeniden başlatın.)",
            L"ScreenLance – Dil Paketi Eksik", MB_ICONWARNING | MB_YESNO);

        if (choice == IDYES)
            OCREngine::OpenLanguageSettingsPage();
        return E_FAIL;
    }

    // Translation engine
    m_translation = std::make_unique<TranslationEngine>(cfg.translation, m_modelDir);
    if (FAILED(m_translation->Init()))
    {
        Logger::Error("Translation init failed – cannot start.");
        ::MessageBoxW(nullptr,
            L"Çeviri modeli yüklenemedi. Models klasöründeki dosyaları kontrol edin.",
            L"ScreenLance – Hata", MB_ICONERROR | MB_OK);
        return E_FAIL;
    }
    m_translation->LoadCache(m_translationCachePath);

    // Capture engine
    m_capture = std::make_unique<CaptureEngine>(cfg.capture);
    if (FAILED(m_capture->Init()))
    {
        Logger::Error("Capture init failed.");
        return E_FAIL;
    }

    // Compute virtual desktop bounds
    RECT virtualRect{};
    for (auto& r : m_capture->GetMonitorRects())
    {
        virtualRect.left   = std::min(virtualRect.left,   r.left);
        virtualRect.top    = std::min(virtualRect.top,    r.top);
        virtualRect.right  = std::max(virtualRect.right,  r.right);
        virtualRect.bottom = std::max(virtualRect.bottom, r.bottom);
    }

    // Overlay engine
    m_overlay = std::make_unique<OverlayEngine>(cfg.overlay, m_hInstance);
    if (FAILED(m_overlay->Init(virtualRect)))
    {
        Logger::Error("Overlay init failed.");
        return E_FAIL;
    }
    // Yatay taşma sınırlamasının (bkz. OverlayEngine::DrawRegion) her
    // metni KENDİ monitörüyle sınırlı tutabilmesi için fiziksel monitör
    // sınırlarını bildir – aksi halde monitör 1'e yakın uzun bir çeviri,
    // overlay tüm sanal masaüstünü kapladığı için monitör 2'nin üzerine
    // taşabilir.
    m_overlay->SetMonitorRects(m_capture->GetMonitorRects());

    // Watchdogs
    m_captureWatchdog = std::make_unique<Watchdog>(
        "CaptureEngine", std::chrono::seconds(30), 5, std::chrono::seconds(300),
        [this]{
            // KULLANICI BİLDİRİMİ: "pencere modunda çalıştı, tam ekranda
            // çalışmadı". Kök neden: Desktop Duplication API (sürekli modun
            // TEK yakalama yöntemi), oyun GERÇEK/TAM (exclusive) tam ekran
            // modundaysa hiçbir zaman yeni bir kare ALAMAZ – çünkü oyun,
            // masaüstü kompozitörünü (DWM) tamamen atlayıp doğrudan ekrana
            // basar, Desktop Duplication ise yalnızca DWM'nin kompoze ettiği
            // çıktıyı yakalayabilir. Bu, Windows/DXGI'nin KENDİ sınırlaması –
            // bizim tarafımızdan giderilemez. Watchdog'un tek başına
            // Restart() çağırması bu durumda İŞE YARAMAZ (aynı yöntemi
            // tekrar dener, sonuç yine boş kalır) ve kullanıcıya HİÇBİR
            // açıklama sunmadan sessizce döngüye girer – tam da bu şikâyete
            // yol açan şeydi. Şimdi her restart denemesinde AÇIK bir uyarı
            // veriyoruz ki kullanıcı "hiçbir şey olmuyor" yerine NEDENİNİ ve
            // çözümünü (oyunu Kenarlıksız/Pencereli moda almak, ya da .exe
            // uyumluluk ayarlarında "Tam ekran iyileştirmelerini devre dışı
            // bırak" işaretliyse kaldırmak) görsün.
            Logger::Warning(
                "CaptureEngine: 30 saniyedir yeni kare yok – hedef uygulama GERÇEK/TAM "
                "(exclusive) tam ekran modda olabilir. Desktop Duplication API, masaüstü "
                "kompozitörünü (DWM) atlayan tam ekran uygulamaları YAKALAYAMAZ (Windows'un "
                "kendi sınırlaması, ScreenLance'ten bağımsız). Çözüm: oyunu/uygulamayı "
                "'Kenarlıksız Pencere' (Borderless/Windowed) moduna al, ya da .exe dosyasının "
                "uyumluluk ayarlarında 'Tam ekran iyileştirmelerini devre dışı bırak' "
                "işaretliyse kaldır. Yeniden başlatma deneniyor...");
            m_capture->Restart();
        });

    m_ocrWatchdog = std::make_unique<Watchdog>(
        "OCREngine", std::chrono::seconds(60), 5, std::chrono::seconds(300),
        [this]{ m_ocr->Restart(); });

    // Çeviri worker'ları uygulama boyunca kalıcı olarak çalışır – yalnızca
    // sürekli moda bağlı DEĞİL. Tek seferlik mod da ARTIK asenkron kuyruğu
    // (m_translateQueue) kullandığı için (bkz. RunOneShotOnRegion), bu
    // worker'ların her zaman ayakta olması gerekiyor; aksi halde sadece
    // tek seferlik mod kullanılan bir oturumda kuyruğu boşaltacak hiçbir
    // thread olmazdı.
    StartTranslationWorkers();

    Logger::Info("All components initialised.");
    return S_OK;
}

void Application::ShutdownComponents()
{
    StopContinuous();

    // Tek seferlik çeviri hâlâ ekranda gösteriliyorsa (8s gösterim penceresi
    // veya OCR/çeviri devam ediyorsa) subsystem'leri yok etmeden önce kısa
    // bir süre bekle – aksi halde arka plan thread'i artık var olmayan
    // m_ocr/m_translation/m_overlay'e erişmeye çalışabilir.
    {
        int waitedMs = 0;
        constexpr int kMaxWaitMs = 9000;
        while (m_oneShotInFlight.load(std::memory_order_relaxed) > 0 && waitedMs < kMaxWaitMs)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waitedMs += 100;
        }
        if (m_oneShotInFlight.load(std::memory_order_relaxed) > 0)
            Logger::Warning("Shutdown: one-shot işlemi zaman aşımına rağmen hâlâ sürüyor.");
    }

    // Çeviri worker'ları artık uygulama boyunca kalıcı (bkz. InitComponents) –
    // kapanışta burada durduruluyor.
    StopTranslationWorkers();

    // KRİTİK SIRALAMA HATASI (çökme kaynağı) #1: m_ocrWatchdog'un zaman
    // aşımı callback'i `m_ocr->Restart()` çağırır. m_ocr'ı watchdog'lardan
    // ÖNCE yok edersek, aradaki dar zaman penceresinde watchdog ateşlenip
    // artık null olan m_ocr üzerinden çağrı yapabilir ("this nullptr idi").
    // Bu yüzden watchdog'ları HER ZAMAN m_ocr'dan ÖNCE durduruyoruz.
    m_captureWatchdog.reset();
    m_ocrWatchdog.reset();

    // KRİTİK SIRALAMA HATASI (çökme kaynağı) #2: m_ocr kendi arka plan iş
    // parçacığında çalışır ve OCR sonucu geldiğinde
    // Application::ProcessOcrResults() -> FlushOverlayMap() ->
    // m_overlay->UpdateRegions(...) çağrısını YAPAR. StopContinuous() yeni
    // kare akışını durdursa da, OCR kuyruğunda (en fazla birkaç öğe) HÂLÂ
    // İŞLENMEKTE olan bir sonuç olabilir – özellikle sistem yoğun
    // kaydırmayla zorlanmışsa (OCR/çeviri kuyruğu birikmişse) bu iş
    // parçacığının o an aktif olma ihtimali belirgin şekilde artar. Önceden
    // m_overlay (ve m_capture, m_translation) BU iş parçacığı hâlâ
    // çalışırken (m_ocr en SONDA reset ediliyordu) yok ediliyordu – bu da
    // OCR iş parçacığının artık yok edilmiş/null bir m_overlay'e erişmesine
    // ("this nullptr idi" yazma erişimi ihlali) yol açabiliyordu. ÇÖZÜM:
    // m_ocr'ı diğer bileşenlerden (m_overlay, m_capture, m_translation)
    // ÖNCE, kendi worker thread'ini (yıkıcısı içinde join() ederek)
    // tamamen durdurup hiçbir callback'in artık tetiklenemeyeceğini
    // GARANTİ ettikten SONRA yok ediyoruz.
    m_ocr.reset();

    if (m_translation)
        m_translation->SaveCache(m_translationCachePath);

    if (m_config) m_config->Save();

    m_hotkeys.reset();
    m_tray.reset();
    m_overlay.reset();
    m_capture.reset();
    m_translation.reset();
    m_config.reset();

    if (m_msgHwnd) { ::DestroyWindow(m_msgHwnd); m_msgHwnd = nullptr; }
}

// ─── Mode control ────────────────────────────────────────────────────────────
void Application::StartContinuous()
{
    if (m_mode == AppMode::ContinuousRunning) return;

    RECT region{};
    if (m_config->Get().capture.areaMode == CaptureAreaMode::FullScreen)
    {
        // "Tam Ekran" modu: kullanıcıya fareyle seçtirmeden, yapılandırılan
        // monitör(ler)in tamamını kullan.
        region = ComputeFullScreenRegion();
        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Warning("Sürekli mod: Tam Ekran alanı hesaplanamadı (monitör bulunamadı), başlatılmadı.");
            return;
        }
        Logger::InfoF("Sürekli mod: Tam Ekran alanı kullanılıyor ({},{})-({},{})",
                      region.left, region.top, region.right, region.bottom);
    }
    else if (m_config->Get().capture.areaMode == CaptureAreaMode::ActiveMonitor)
    {
        // "Aktif Monitör" modu: yalnızca o an fare imlecinin üzerinde
        // olduğu monitörü kullan – farklı çözünürlükteki diğer monitör(ler)
        // bu bölgenin tamamen dışında kalır ve OnFrame/CropFrameToRegion
        // tarafından baştan elenir, dolayısıyla OCR/çeviri/overlay onlara
        // hiç dokunmaz.
        region = ComputeActiveMonitorRegion();
        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Warning("Sürekli mod: Aktif Monitör alanı hesaplanamadı, başlatılmadı.");
            return;
        }
        Logger::InfoF("Sürekli mod: Aktif Monitör alanı kullanılıyor ({},{})-({},{})",
                      region.left, region.top, region.right, region.bottom);
    }
    else
    {
        // SelectionWindow kendi mesaj döngüsünü çalıştırır ve kullanıcı
        // bırakana/ESC'ye basana kadar bloke olur (TriggerOneShot ile aynı desen).
        Logger::Info("Sürekli mod: alan seçimi bekleniyor...");
        SelectionWindow selector(m_hInstance);
        region = selector.Show();

        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Info("Sürekli mod: seçim iptal edildi, başlatılmadı.");
            return;
        }
        Logger::InfoF("Sürekli mod: seçilen alan ({},{})-({},{})",
                      region.left, region.top, region.right, region.bottom);
    }

    m_continuousRegion = region;
    m_lastFrameHash.clear(); // önceki oturumdan kalan hash yeni ilk kareyi yanlışlıkla atlamasın

    m_mode = AppMode::ContinuousRunning;
    m_tray->SetContinuousRunning(true);

    m_captureWatchdog->Start();
    m_ocrWatchdog->Start();

    m_capture->Start([this](CaptureFrame f){ OnFrame(std::move(f)); });
    Logger::Info("Continuous mode started.");
}

void Application::StopContinuous()
{
    if (m_mode != AppMode::ContinuousRunning) return;
    m_mode = AppMode::Idle;
    m_tray->SetContinuousRunning(false);

    m_captureWatchdog->Stop();
    m_ocrWatchdog->Stop();
    m_capture->Stop();

    // ÖNEMLİ HATA DÜZELTMESİ: m_overlay->ClearAll() yalnızca OverlayEngine'in
    // KENDİ görsel listesini temizler. Ama bu uygulamanın "kaynak" durumu
    // m_overlayMap'tir (bkz. FlushOverlayMap) – ClearAll çağrısı bunu
    // ETKİLEMİYORDU. Sonuç: sürekli mod durdurulduktan sonra m_overlayMap
    // içinde hâlâ eski bölgeler duruyordu; bir sonraki FlushOverlayMap()
    // çağrısı (örn. hemen ardından başlatılan tek seferlik çevirinin İLK
    // sonucu geldiğinde) map'teki HER ŞEYİ yeniden render ediyordu –
    // eski, artık geçersiz sürekli-mod çevirileri de dahil olmak üzere.
    // Bu, "sürekli çeviriyi durdurup one-shot başlatınca eski çeviri ekranda
    // takılı kalıyor" şikâyetinin doğrudan nedeniydi. Şimdi map de burada
    // temizleniyor.
    {
        std::lock_guard lock(m_overlayMutex);
        m_overlayMap.clear();
        m_overlayGeneration.fetch_add(1, std::memory_order_release); // bkz. Application.h yorumu
        // Kuyrukta hâlâ işlenmekte olan bir sürekli-mod çevirisi varsa,
        // tamamlandığında TranslationWorkerLoop'taki bayatlık kontrolü
        // `it != m_latestRequestedText.end()` bulup bunu GEÇERLİ sanabilir
        // (bu map az önce silinen m_overlayMap'ten BAĞIMSIZ ayrı bir
        // yapı). Bu da az önce temizlediğimiz eski çevirinin, tam da bu
        // kısa yarış penceresinde geri gelmesine yol açardı. Bu yüzden
        // aynı kilit altında bunu da temizliyoruz – artık kuyrukta kalan
        // her sonuç `it == end()` bulup bayat sayılıp atlanacak.
        m_latestRequestedText.clear();
    }
    m_overlay->ClearAll();
    Logger::Info("Continuous mode stopped.");
}

void Application::TriggerOneShot()
{
    Logger::Info("One-shot mode triggered.");

    RECT region{};
    if (m_config->Get().capture.areaMode == CaptureAreaMode::FullScreen)
    {
        // "Tam Ekran" modu: kullanıcıya fareyle seçtirmeden, yapılandırılan
        // monitör(ler)in tamamını kullan.
        region = ComputeFullScreenRegion();
        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Warning("One-shot: Tam Ekran alanı hesaplanamadı (monitör bulunamadı).");
            return;
        }
    }
    else if (m_config->Get().capture.areaMode == CaptureAreaMode::ActiveMonitor)
    {
        // "Aktif Monitör" modu: fare imlecinin üzerinde olduğu monitörle
        // sınırlı kal, diğer monitöre hiç dokunma.
        region = ComputeActiveMonitorRegion();
        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Warning("One-shot: Aktif Monitör alanı hesaplanamadı.");
            return;
        }
    }
    else
    {
        // SelectionWindow kendi mesaj döngüsünü çalıştırır ve kullanıcı
        // bırakana/ESC'ye basana kadar bloke olur. Application'ın ana döngüsü
        // bu sırada durur (modal pencere gibi) – Show() döner dönmez normale
        // devam eder, bu yüzden burada bloklamak güvenli ve beklenen davranış.
        SelectionWindow selector(m_hInstance);
        region = selector.Show();

        bool isEmpty = (region.left >= region.right) || (region.top >= region.bottom);
        if (isEmpty)
        {
            Logger::Info("One-shot: seçim iptal edildi.");
            return;
        }
    }

    // Yakalama + OCR + çeviri + overlay'i arka plan thread'inde çalıştır;
    // mesaj döngüsü (hotkey/tray) bu sırada tamamen yanıt vermeye devam eder.
    std::thread([this, region]{ RunOneShotOnRegion(region); }).detach();
}

RECT Application::ComputeFullScreenRegion() const
{
    RECT region{};
    if (!m_capture) return region;
    for (auto& r : m_capture->GetMonitorRects())
    {
        region.left   = std::min(region.left,   r.left);
        region.top    = std::min(region.top,    r.top);
        region.right  = std::max(region.right,  r.right);
        region.bottom = std::max(region.bottom, r.bottom);
    }
    return region;
}

RECT Application::ComputeActiveMonitorRegion() const
{
    RECT region{};
    if (!m_capture) return region;

    // "Aktif monitör" tespiti: ÖNCELİKLE ön plandaki (foreground) pencerenin
    // bulunduğu monitör kullanılır. Bunun nedeni: hotkey'ler klavyeden
    // tetiklenir ve kullanıcı kısayola basarken fareyi genelde HİÇ
    // hareket ettirmez – fare imleci önceki bir işten kalma şekilde başka
    // bir monitörde duruyor olabilir. Yalnızca imleç konumuna bakmak, bu
    // durumda kullanıcının o an gerçekte çalıştığı ekranla ilgisiz bir
    // sonuç verir (ara sıra yanlış monitörün çevrildiği, sonra one-shot'ın
    // 8s gösterim süresi dolunca "aniden kaybolan" davranışın kök nedeni
    // buydu). Yalnızca geçerli bir foreground pencere yoksa (örn. hiçbir
    // pencere odakta değilse) fare imleci konumuna düşülür.
    HMONITOR hMon = nullptr;
    if (HWND fg = ::GetForegroundWindow())
        hMon = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);

    if (!hMon)
    {
        POINT pt{};
        ::GetCursorPos(&pt);
        hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (hMon && ::GetMonitorInfoW(hMon, &mi))
    {
        // CaptureEngine'in oluşturduğu duplication bağlamlarından biriyle
        // eşleştir (yalnızca m_capture'ın bildiği monitörler yakalanabilir).
        // DPI/Win32 yuvarlaması nedeniyle sınırlar birebir aynı olmayabilir,
        // bu yüzden en çok örtüşen dikdörtgeni seçiyoruz.
        long bestOverlap = -1;
        for (auto& r : m_capture->GetMonitorRects())
        {
            RECT overlap{};
            if (::IntersectRect(&overlap, &r, &mi.rcMonitor))
            {
                long area = (overlap.right - overlap.left) * (overlap.bottom - overlap.top);
                if (area > bestOverlap)
                {
                    bestOverlap = area;
                    region = r;
                }
            }
        }
        if (bestOverlap < 0)
            region = mi.rcMonitor; // Eşleşme bulunamadıysa Win32'nin verdiği sınırlara düş
    }
    else
    {
        // Son çare: ne foreground pencere ne de imleç konumu alınabildiyse
        // yine de tek bir monitörle sınırlı kalmak için ilk yapılandırılmış
        // monitörü kullan (tüm monitörlere yayılan eski davranışa dönmemek
        // için).
        auto& rects = m_capture->GetMonitorRects();
        if (!rects.empty()) region = rects.front();
    }

    Logger::InfoF("Aktif monitör bölgesi: ({},{})-({},{})",
                  region.left, region.top, region.right, region.bottom);
    return region;
}

// ─── Tek seferlik çeviri: bölge yakalama (GDI) ───────────────────────────────
CaptureFrame Application::CaptureRegionGDI(const RECT& region) const
{
    CaptureFrame frame;
    int w = region.right  - region.left;
    int h = region.bottom - region.top;
    if (w <= 0 || h <= 0) return frame;

    HDC screenDC = ::GetDC(nullptr);
    HDC memDC    = ::CreateCompatibleDC(screenDC);
    HBITMAP bmp  = ::CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ old  = ::SelectObject(memDC, bmp);

    // CAPTUREBLT: katmanlı (layered) pencereleri de yakalamaya dahil eder –
    // overlay'imiz zaten WDA_EXCLUDEFROMCAPTURE ile bu yakalamadan hariç
    // tutuluyor, bu bayrak diğer normal layered pencerelerin (ör. başka
    // uygulamaların şeffaf pencereleri) doğru yakalanması için.
    ::BitBlt(memDC, 0, 0, w, h, screenDC, region.left, region.top, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h; // Üstten-alta (top-down) DIB
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    frame.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    ::GetDIBits(memDC, bmp, 0, h, frame.bgra.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(memDC, old);
    ::DeleteObject(bmp);
    ::DeleteDC(memDC);
    ::ReleaseDC(nullptr, screenDC);

    frame.width            = w;
    frame.height           = h;
    frame.virtualRect      = region;  // OCR, bounding box'ları buna göre offsetler
    frame.monitorIndex     = -1;      // Tek seferlik modda kullanılmıyor
    frame.hasDesktopUpdate = true;
    return frame;
}

// ─── Desktop Duplication ile tek seferlik bölge yakalama ──────────────────────
// GDI BitBlt (CaptureRegionGDI), DirectX/OpenGL/Vulkan ile render edilen
// modern oyun/uygulama içeriğini genelde yakalayamaz (siyah veya eski/donmuş
// bir görüntü döner) – çünkü GPU-composite edilen yüzeyler klasik GDI
// belleğine yansımaz. NOT: Burada YENİ bir IDXGIOutputDuplication OLUŞTURMUYORUZ
// – Windows aynı monitör çıktısı için aynı anda yalnızca TEK bir aktif
// duplication'a izin verir, ve m_capture (sürekli mod) zaten kendi
// duplication handle'ını uygulama başlangıcından beri açık tutuyor. Bunun
// yerine CaptureEngine::CaptureRegionOnce() ile O HANDLE'I yeniden
// kullanıyoruz (thread-safe, mutex ile korunuyor).
CaptureFrame Application::CaptureRegionDDA(const RECT& region) const
{
    CaptureFrame frame;
    if (m_capture)
    {
        HRESULT hr = m_capture->CaptureRegionOnce(region, frame);
        if (SUCCEEDED(hr) && !frame.bgra.empty())
            return frame;

        Logger::WarningF("One-shot: Desktop Duplication yakalaması başarısız (0x{:08X}) – oyun TAM "
                          "EKRAN (exclusive fullscreen) modda olabilir; bu durumda hiçbir yakalama "
                          "API'si çalışmaz. Oyunu 'Pencere Kenarlıksız' (Borderless/Windowed) moda alıp "
                          "tekrar deneyin. GDI yakalamaya geri düşülüyor.", static_cast<unsigned long>(hr));
    }
    return CaptureRegionGDI(region);
}

// ─── Tek seferlik çeviri: tam akış ────────────────────────────────────────────
void Application::RunOneShotOnRegion(RECT region)
{
    Logger::InfoF("One-shot: bölge yakalanıyor ({},{})-({},{})",
                  region.left, region.top, region.right, region.bottom);

    CaptureFrame frame = CaptureRegionDDA(region);
    if (frame.bgra.empty())
    {
        Logger::Warning("One-shot: bölge yakalama başarısız (boş kare).");
        return;
    }

    // OCREngine'in asenkron callback'ini promise/future ile senkron bekleyişe
    // dönüştür – continuous mode ile aynı OCR worker'ı/kuyruğu yeniden
    // kullanır, ayrı bir kod yolu icat etmeye gerek bırakmaz.
    auto resultPromise = std::make_shared<std::promise<std::vector<OcrRegion>>>();
    std::future<std::vector<OcrRegion>> resultFuture = resultPromise->get_future();

    m_ocr->SubmitFrame(std::move(frame),
        [resultPromise](std::vector<OcrRegion> regions){
            resultPromise->set_value(std::move(regions));
        });

    std::vector<OcrRegion> regions;
    if (resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
        regions = resultFuture.get();
    else
        Logger::Warning("One-shot: OCR 5 saniye içinde yanıt vermedi (zaman aşımı).");

    Logger::InfoF("One-shot: OCR {} bölge buldu.", regions.size());
    if (regions.empty())
    {
        Logger::Info("One-shot: seçilen alanda İngilizce metin bulunamadı.");
        return;
    }

    // ── Çevirileri ASENKRON kuyruğa gönder ───────────────────────────────
    // ÖNCEKİ TASARIM burada m_translation->Translate() çağrısını bir
    // for döngüsünde SENKRON (bloklayarak) çalıştırıyordu – yani 3 bölge
    // varsa, İLKİNİN çevirisi 3 saniyede hazır olsa bile kullanıcı
    // SONUNCUSU bitene kadar (10-40+ saniye) ekranda hiçbir şey
    // göremiyordu. Artık sürekli modun zaten kullandığı asenkron kuyruğa
    // (m_translateQueue) gönderiliyor; her bölgenin çevirisi HAZIR OLDUĞU
    // AN, diğerlerini beklemeden ayrı ayrı ekrana yansıyor (bkz.
    // TranslationWorkerLoop'taki tek-seferlik otomatik-kaldırma mantığı).
    {
        std::lock_guard lockQ(m_translateQueueMutex);
        std::lock_guard lockS(m_oneShotIdsMutex);
        for (auto& r : regions)
        {
            m_translateQueue.push(r);
            m_oneShotIds.insert(r.id);
            m_oneShotInFlight.fetch_add(1, std::memory_order_relaxed);
        }
    }
    m_translateQueueCv.notify_all();
    Logger::InfoF("One-shot: {} bölge çeviri kuyruğuna eklendi.", regions.size());
}

// ─── Frame pipeline ──────────────────────────────────────────────────────────
std::vector<uint8_t> Application::HashFrameSample(const CaptureFrame& frame)
{
    if (frame.bgra.empty() || frame.width <= 0 || frame.height <= 0)
        return {};

    std::vector<uint8_t> samples;
    samples.reserve(32 * 32 * 3);
    int stepY = std::max(1, frame.height / 32);
    int stepX = std::max(1, frame.width  / 32);
    for (int y = 0; y < frame.height; y += stepY)
    for (int x = 0; x < frame.width;  x += stepX)
    {
        size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(frame.width)
                    + static_cast<size_t>(x)) * 4;
        samples.push_back(frame.bgra[idx]);
        samples.push_back(frame.bgra[idx + 1]);
        samples.push_back(frame.bgra[idx + 2]);
    }
    return samples;
}

bool Application::CropFrameToRegion(const CaptureFrame& src, const RECT& region, CaptureFrame& out) const
{
    RECT overlap{};
    if (!::IntersectRect(&overlap, &src.virtualRect, &region))
        return false;

    int rx0 = overlap.left - src.virtualRect.left;
    int ry0 = overlap.top  - src.virtualRect.top;
    int w   = overlap.right  - overlap.left;
    int h   = overlap.bottom - overlap.top;
    if (w <= 0 || h <= 0 || src.bgra.empty())
        return false;

    out.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int row = 0; row < h; ++row)
    {
        const uint8_t* srcRow = src.bgra.data()
            + (static_cast<size_t>(ry0 + row) * static_cast<size_t>(src.width) + static_cast<size_t>(rx0)) * 4;
        std::memcpy(out.bgra.data() + static_cast<size_t>(row) * static_cast<size_t>(w) * 4,
                    srcRow, static_cast<size_t>(w) * 4);
    }
    out.width            = w;
    out.height           = h;
    out.virtualRect       = overlap;
    out.monitorIndex      = src.monitorIndex;
    out.hasDesktopUpdate  = src.hasDesktopUpdate;
    return true;
}

void Application::OnFrame(CaptureFrame frame)
{
    m_captureWatchdog->Ping();

    // Sürekli mod: yalnızca kullanıcının seçtiği alt-bölgeyi işle (tüm
    // monitörü değil) – hem gereksiz OCR yükünü azaltır hem de "Sürekli
    // çeviri için alan seçimi çıkmadı" sorununu çözer (artık gerçekten
    // seçilen alan kullanılıyor).
    CaptureFrame cropped;
    if (!CropFrameToRegion(frame, m_continuousRegion, cropped))
        return; // seçilen alan bu monitörle örtüşmüyor

    // OCR rate limiting
    using rep = clock_t::time_point::rep;
    auto& cfg = m_config->Get();
    int ocrIntervalMs = 1000 / std::max(1, cfg.capture.ocrFps);

    auto now    = clock_t::now().time_since_epoch().count();
    auto last   = m_lastOcrSubmit.load(std::memory_order_relaxed);
    auto diffMs = (now - last) / 1'000'000;

    if (diffMs < ocrIntervalMs) return;
    m_lastOcrSubmit.store(now, std::memory_order_relaxed);

    // ── Değişmeyen kareyi atla (en büyük tek performans kazancı) ────────────
    // Windows OCR (RecognizeAsync) pipeline'ın en pahalı adımıdır. Ekran
    // içeriği son gönderilen kareyle AYNIYSA (örn. duran bir menü/diyalog),
    // OCR'ı tekrar tekrar çalıştırmanın hiçbir faydası yok – yalnızca ucuz
    // bir örnekleme (32x32'ye kadar piksel) ile karşılaştırıyoruz.
    auto hash = HashFrameSample(cropped);
    if (!hash.empty() && hash == m_lastFrameHash)
        return; // ekran değişmedi, pahalı OCR çağrısı atlandı
    m_lastFrameHash = hash;

    m_ocr->SubmitFrame(std::move(cropped),
        [this](std::vector<OcrRegion> regions){
            ProcessOcrResults(std::move(regions));
        });
}

void Application::ProcessOcrResults(std::vector<OcrRegion> newRegions)
{
    m_ocrWatchdog->Ping();

    std::vector<OcrRegion> oldRegions;
    {
        std::lock_guard lock(m_regionMutex);
        oldRegions = m_currentRegions;
    }

    auto delta = DiffRegions(oldRegions, newRegions);

    Logger::DebugF("OCR: {} bölge toplam ({} yeni, {} değişti, {} kayboldu)",
                   newRegions.size(), delta.added.size(),
                   delta.changed.size(), delta.removedIds.size());

    // m_currentRegions'ı ID'si DÜZELTİLMİŞ bölge listesiyle güncelle (ham
    // newRegions DEĞİL). DiffRegions eşleşen bölgeler için kalıcı/eski ID'yi
    // korur (bkz. DiffRegions); bunu burada da taşımazsak bir sonraki OCR
    // turu yine bu turun HAM (taze) ID'sini "kalıcı" sanıp aynı üst-üste
    // birikme hatasını zincirleme üretir.
    std::vector<OcrRegion> corrected;
    corrected.reserve(delta.added.size() + delta.changed.size() + delta.unchanged.size());
    for (auto& r : delta.added)     corrected.push_back(r);
    for (auto& r : delta.changed)   corrected.push_back(r);
    for (auto& r : delta.unchanged) corrected.push_back(r);

    {
        std::lock_guard lock(m_regionMutex);
        m_currentRegions = std::move(corrected);
    }

    // Remove overlay regions that disappeared VEYA metni değişti. Değişen
    // bölgenin ESKİ çevirisi artık ekrandaki YENİ metinle eşleşmiyor (yanlış/
    // bayat bir çeviri gösterirdi) – bu yüzden yeni çeviri hazır olana kadar
    // hemen kaldırılıyor; TranslationWorkerLoop yeni çeviri bitince aynı
    // (kalıcı) ID ile tekrar ekleyip FlushOverlayMap() çağıracak.
    {
        std::lock_guard lock(m_overlayMutex);
        for (uint64_t id : delta.removedIds)
        {
            m_overlayMap.erase(id);
            m_latestRequestedText.erase(id); // artık ekranda yok, takibe gerek yok
        }
        for (auto& r : delta.changed)
            m_overlayMap.erase(r.id);
        m_overlayGeneration.fetch_add(1, std::memory_order_release); // tanılama/log amaçlı
        // Render'ı BURADA, AYNI kilit altında yapıyoruz (bkz.
        // RenderOverlayMapLocked yorumu – mutasyon+render'ın atomik olması
        // için ayrı bir FlushOverlayMap() çağrısı BEKLEMİYORUZ).
        RenderOverlayMapLocked();
    }

    // Enqueue changed/added regions for translation. Aynı zamanda BU ID için
    // EN SON istenen kaynak metni kaydediyoruz – çeviri (birkaç saniye
    // sürebilir) tamamlandığında, hâlâ "en güncel istek" olup olmadığını
    // kontrol edip bayat sonuçları atmak için (bkz. TranslationWorkerLoop).
    {
        std::lock_guard lockQ(m_translateQueueMutex);
        std::lock_guard lockO(m_overlayMutex);

        // Kuyruk BOYUT SINIRI: sistem ağır yük altındayken (örn. uzun süreli
        // hızlı kaydırma) OCR, LLM çeviriyi tüketebildiğinden çok daha hızlı
        // yeni bölge üretebilir. Sınırsız bir kuyruk, giderek ARTAN sayıda
        // BAYAT isteğin (ekrandan çoktan kaybolmuş metinler için) birikmesine
        // yol açar – bu da "çiftlemeler/üst üste binmeler" şikayetine katkıda
        // bulunuyordu (isStale kontrolü artık bunları doğru şekilde eliyor,
        // bkz. TranslationWorkerLoop, ama işlenmeyi BEKLERKEN kuyruğun
        // gereksiz yere şişmesini/gecikmeyi büyütmesini de engellemek daha
        // sağlıklı). Kapasite aşılırsa EN ESKİ (muhtemelen zaten bayat) kayıt
        // atılır; bu, ID bazlı değil basit bir FIFO sınırlamasıdır ama
        // pratikte en eski isteklerin en bayat olma ihtimali en yüksektir.
        constexpr size_t kMaxQueueSize = 24;
        auto trimQueue = [this] {
            while (m_translateQueue.size() > kMaxQueueSize)
                m_translateQueue.pop();
        };

        for (auto& r : delta.added)
        {
            m_translateQueue.push(r);
            m_latestRequestedText[r.id] = r.text;
            trimQueue();
        }
        for (auto& r : delta.changed)
        {
            m_translateQueue.push(r);
            m_latestRequestedText[r.id] = r.text;
            trimQueue();
        }
    }
    m_translateQueueCv.notify_all();
}

// ─── Change detection ────────────────────────────────────────────────────────
// ─── Metin benzerliği (kaydırma sırasında yanlış kimlik devrini önlemek için) ─
// İki dizi arasındaki normalize edilmiş Levenshtein (düzenleme) mesafesini
// hesaplar: 0 = birebir aynı, 1 = tamamen alakasız. Kısa UI metinleri için
// (birkaç yüz karaktere kadar) O(n*m) DP performans açısından önemsizdir –
// bu, OCR turu başına birkaç kez (saniyede birkaç kez) çağrılır, 60 FPS'lik
// bir maliyet DEĞİLDİR.
static float NormalisedEditDistance(const std::wstring& a, const std::wstring& b)
{
    if (a == b) return 0.f;
    size_t la = a.size(), lb = b.size();
    if (la == 0 || lb == 0) return 1.f;

    std::vector<size_t> prev(lb + 1), cur(lb + 1);
    for (size_t j = 0; j <= lb; ++j) prev[j] = j;

    for (size_t i = 1; i <= la; ++i)
    {
        cur[0] = i;
        for (size_t j = 1; j <= lb; ++j)
        {
            size_t cost = (towlower(a[i - 1]) == towlower(b[j - 1])) ? 0 : 1;
            cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    size_t dist = prev[lb];
    return static_cast<float>(dist) / static_cast<float>((std::max)(la, lb));
}

Application::RegionDelta Application::DiffRegions(
    const std::vector<OcrRegion>& oldR, const std::vector<OcrRegion>& newR) const
{
    constexpr float IOUThreshold  = 0.7f;
    // Kaydırma (scroll) sırasında ekrandaki TÜM metin bloklarının konumu
    // aynı anda değişir. Bu durumda eski bir bloğun bıraktığı konum, tamamen
    // ALAKASIZ yeni bir metin bloğuyla GEÇİCİ olarak örtüşebilir (yüksek
    // IoU) – yalnızca konuma bakarsak bunu "aynı öğenin metni değişti"
    // sanıp eski (kalıcı) ID'yi bu alakasız yeni metne devrederdik. Bu da
    // tam olarak bildirilen hataydı: kaydırma sırasında eski ve yeni
    // çeviriler birbirine karışıyordu. Çözüm: ID'yi yalnızca metinler
    // GERÇEKTEN İLİŞKİLİYSE koru (aynıysa, ya da örn. canlı güncellenen bir
    // sayaç/skor gibi küçük bir farkla değiştiyse – "Skor: 41" -> "Skor: 42"
    // düzenleme mesafesi açısından KÜÇÜK bir farktır). Metin tamamen
    // alakasızsa (kaydırmayla farklı bir blok o konuma gelmiş), bunu eski
    // bloğun KAYBOLMASI + yepyeni bir bloğun EKLENMESİ olarak ele alıyoruz –
    // bu da eski ID'nin yanlışlıkla alakasız içeriğe devredilmesini
    // engelliyor.
    constexpr float kMaxEditDistanceToKeepId = 0.5f;
    RegionDelta delta;

    std::vector<bool> matched(oldR.size(), false);

    for (auto& n : newR)
    {
        float bestIou = 0.f;
        int   bestIdx = -1;

        for (int i = 0; i < static_cast<int>(oldR.size()); ++i)
        {
            float iou = CalculateIoU(n.bounds, oldR[i].bounds);
            if (iou > bestIou) { bestIou = iou; bestIdx = i; }
        }

        bool sameText = (bestIdx >= 0) && (n.text == oldR[bestIdx].text);
        bool related  = sameText ||
            (bestIdx >= 0 && NormalisedEditDistance(n.text, oldR[bestIdx].text) <= kMaxEditDistanceToKeepId);

        if (bestIdx >= 0 && bestIou >= IOUThreshold && related)
        {
            matched[bestIdx] = true;

            // KRİTİK: Aynı fiziksel konum için ESKİ (kalıcı) ID'yi koru.
            // OCREngine her turda NewID() ile TAZE bir ID üretiyor; bunu
            // olduğu gibi bırakırsak m_overlayMap[tr.id] her seferinde
            // YENİ bir giriş ekler (eskisi hiç silinmez, çünkü IoU
            // eşleşmesi "kayboldu" listesine girmesini engeller) – bu da
            // aynı konumda çevirilerin üst üste binmesine yol açıyordu.
            // ID'yi eskiyle değiştirerek harita güncellemesi artık
            // EKLEME değil ÜZERİNE YAZMA oluyor.
            OcrRegion nn = n;
            nn.id = oldR[bestIdx].id;

            // Same position: check if text changed
            if (sameText)
                delta.unchanged.push_back(nn);
            else
                delta.changed.push_back(nn);
        }
        else
        {
            // bestIdx eşleşmedi VEYA konum örtüşse bile metin alakasız
            // (kaydırma) – tamamen YENİ bir kimlikle ekle. Eski aday
            // "matched" işaretlenmediği için aşağıda otomatik olarak
            // kaybolanlar listesine düşecek.
            delta.added.push_back(n);
        }
    }

    for (int i = 0; i < static_cast<int>(oldR.size()); ++i)
        if (!matched[i])
            delta.removedIds.push_back(oldR[i].id);

    return delta;
}

// ─── Translation worker pool ─────────────────────────────────────────────────
void Application::StartTranslationWorkers()
{
    m_workersRunning.store(true);
    for (int i = 0; i < kTranslationWorkerCount; ++i)
        m_translationWorkers.emplace_back(&Application::TranslationWorkerLoop, this);
}

void Application::StopTranslationWorkers()
{
    m_workersRunning.store(false);
    // Devam eden bir çeviri varsa erken durdur – aksi halde join() çağrısı
    // uzun bir üretimin (maxTokens'a kadar) doğal olarak bitmesini
    // bekleyip arayüzün "kilitlenmiş" gibi görünmesine yol açardı.
    if (m_translation)
        m_translation->RequestCancel();
    m_translateQueueCv.notify_all();
    for (auto& t : m_translationWorkers)
        if (t.joinable()) t.join();
    m_translationWorkers.clear();
    {
        std::lock_guard lock(m_translateQueueMutex);
        while (!m_translateQueue.empty()) m_translateQueue.pop();
    }
}

void Application::TranslationWorkerLoop()
{
    while (true)
    {
        OcrRegion region;
        {
            std::unique_lock lock(m_translateQueueMutex);
            m_translateQueueCv.wait(lock, [&]{
                return !m_translateQueue.empty() || !m_workersRunning.load();
            });
            if (!m_workersRunning.load() && m_translateQueue.empty()) return;
            region = std::move(m_translateQueue.front());
            m_translateQueue.pop();
        }

        std::wstring translated = m_translation->Translate(region.text);

        // Bu bölge tek seferlik moddan mı geldi? (Sürekli moddan gelen
        // bölgeler bu sette hiç yoktur – onların ömrü OCR diff/kaybolma
        // mantığıyla yönetilir, sabit bir süre sonra otomatik silinmezler.)
        bool isOneShotRegion = false;
        {
            std::lock_guard lock(m_oneShotIdsMutex);
            auto oit = m_oneShotIds.find(region.id);
            if (oit != m_oneShotIds.end())
            {
                isOneShotRegion = true;
                m_oneShotIds.erase(oit);
            }
        }

        if (translated.empty())
        {
            Logger::DebugF("Translate: '{}' -> (boş sonuç)", NarrowForLog(region.text));
            if (isOneShotRegion) m_oneShotInFlight.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        Logger::DebugF("Translate: '{}' -> '{}'",
                       NarrowForLog(region.text), NarrowForLog(translated));

        TranslatedRegion tr;
        tr.id             = region.id;
        tr.bounds         = region.bounds;
        tr.originalText   = region.text;
        tr.translatedText = translated;
        tr.dirty          = true;
        tr.hasDetectedColor = region.hasDetectedColor;
        tr.detectedColor    = region.detectedColor;
        tr.detectedFontSize = region.detectedFontSize;
        tr.hasDetectedBgColor = region.hasDetectedBgColor;
        tr.detectedBgColor    = region.detectedBgColor;

        bool isStale = false;
        {
            std::lock_guard lock(m_overlayMutex);
            auto it = m_latestRequestedText.find(tr.id);
            // Bu ID için DAHA YENİ bir istek varsa (ekrandaki metin bu
            // çeviri kuyruğa girdikten SONRA tekrar değiştiyse), bu sonuç
            // artık bayattır – ekrana yazmadan atlıyoruz. Yeni isteğin
            // kendi çevirisi, tamamlandığında doğru sonucu yazacak.
            //
            // ÖNEMLİ AYRIM: Tek seferlik (one-shot) bölgeler
            // m_latestRequestedText'e HİÇ EKLENMEZ (yalnızca sürekli mod
            // bunu kullanır, bkz. RunOneShotOnRegion) – bu yüzden onlar için
            // `it == end()` tamamen NORMALDİR, bayat anlamına GELMEZ.
            //
            // Sürekli mod için ise `it == end()` durumu FARKLI bir anlama
            // gelir: ProcessOcrResults, bir bölge ekrandan kaybolduğunda bu
            // ID'yi m_latestRequestedText'ten SİLER (bkz. delta.removedIds
            // işleme). Kuyrukta uzun süre bekleyen (örn. ağır yük altında
            // kaydırma sırasında biriken) bir çeviri isteği, karşılık
            // geldiği metin ekrandan TAMAMEN kaybolduktan SONRA
            // tamamlanırsa, bunu da BAYAT saymamız gerekir – aksi halde
            // ekrandan kaybolmuş metnin çevirisi "hayalet" gibi geri gelir.
            // Bu, ağır yük altında bildirilen "çiftlemeler/üst üste
            // binmeler"in asıl nedenlerinden biriydi.
            if (isOneShotRegion)
                isStale = (it != m_latestRequestedText.end() && it->second != region.text);
            else
                isStale = (it == m_latestRequestedText.end()) || (it->second != region.text);
            if (!isStale)
            {
                m_overlayMap[tr.id] = std::move(tr);
                m_overlayGeneration.fetch_add(1, std::memory_order_release); // tanılama/log amaçlı
                // KRİTİK: render'ı BURADA, AYNI kilit altında yapıyoruz.
                // Birden fazla çeviri worker thread'i (havuzdaki paralel
                // worker'lar) neredeyse aynı anda kendi sonuçlarını
                // ekleyebiliyor – render çağrısı ayrı/kilitsiz bir adım
                // olsaydı, bir thread'in ESKİ anlık görüntüsü başka bir
                // thread'in YENİ eklediği sonucun üzerine yazabilirdi (bkz.
                // RenderOverlayMapLocked yorumu – "yine takılı kaldı"
                // bildirimine yol açan yarış durumu).
                RenderOverlayMapLocked();
            }
        }

        if (isStale)
        {
            Logger::DebugF("Translate: '{}' için sonuç bayat (metin bu sırada değişmiş), atlanıyor.",
                            NarrowForLog(region.text));
            if (isOneShotRegion) m_oneShotInFlight.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        if (isOneShotRegion)
        {
            // Tek seferlik çeviri: 8 saniye gösterip sonra otomatik kaldır.
            // AYRI, kısa ömürlü bir arka plan thread'inde yapılıyor – ana
            // worker thread'ini BLOKLAMADAN (kuyruktaki diğer bölgeler
            // beklemeden işlenmeye devam etsin diye). ÖNCEKİ tasarımda
            // çeviriler bir for döngüsünde SIRAYLA (senkron) yapılıyor ve
            // overlay yalnızca TÜM bölgeler bitince (tek seferlik)
            // güncelleniyordu – yani ilk bölgenin çevirisi saniyeler
            // içinde hazır olsa bile, kullanıcı SON bölge de bitene kadar
            // (10-40+ saniye) ekranda hiçbir şey göremiyordu.
            int displaySeconds = std::clamp(m_config->Get().overlay.oneShotDisplaySeconds, 1, 60);
            Logger::InfoF("One-shot: '{}' çevirisi ekrana yansıtıldı, {} saniye gösterilecek.",
                           NarrowForLog(region.text), displaySeconds);
            uint64_t idToClear = region.id;
            std::thread([this, idToClear, displaySeconds]{
                std::this_thread::sleep_for(std::chrono::seconds(displaySeconds));
                {
                    // KRİTİK: burada da erase+render AYNI kilit altında,
                    // ara vermeden yapılıyor (bkz. yukarıdaki yorum ve
                    // RenderOverlayMapLocked) – tam olarak BU thread'lerin
                    // (her bölge için bağımsız, neredeyse aynı anda
                    // tetiklenen "N saniye sonra sil" thread'leri) birbirini
                    // yanlış sırada geçmesi "yine takılı kaldı" bildirimine
                    // yol açan asıl nedendi.
                    std::lock_guard lock(m_overlayMutex);
                    m_overlayMap.erase(idToClear);
                    m_overlayGeneration.fetch_add(1, std::memory_order_release); // tanılama/log amaçlı
                    RenderOverlayMapLocked();
                }
                m_oneShotInFlight.fetch_sub(1, std::memory_order_relaxed);
            }).detach();
        }
    }
}

// ─── Overlay render (render çağrısı m_overlayMutex tutulurken yapılır) ───────
// KRİTİK DÜZELTME (2. tur – "yine takılı kaldı" bildirimi): bir önceki
// düzeltme (nesil sayacıyla "bayat mı?" kontrolü) yarış PENCERESİNİ
// daralttı ama TAMAMEN KAPATMADI. Sorun şuydu: kontrol ("myGen güncel mi?")
// ile asıl render çağrısı (m_overlay->UpdateRegions(...)) ARASINDA hâlâ
// kısa bir süre vardı – kontrol geçse bile, işletim sistemi bu thread'i tam
// o anda geciktirip BAŞKA bir (daha yeni nesle sahip) thread'in KENDİ
// kontrolünü geçip render'ını ÖNCE tamamlamasına izin verebiliyordu; sonra
// geciken thread'in ESKİ render'ı onun ÜZERİNE yazıyordu. 5 bölgenin
// neredeyse aynı anda süresi dolduğu bir senaryoda bu, PRATİKTE gerçekten
// tetiklenebilecek kadar sık oluyormuş (bkz. kullanıcının ikinci log'u).
//
// GERÇEK ÇÖZÜM: "kontrol et, sonra render et" gibi İKİ AYRI adım yerine,
// mutasyon (ekleme/silme) VE render'ı TEK BİR kilit bloğu içinde, HİÇ ARA
// VERMEDEN yapmak. Bu fonksiyon bunun için var – ÇAĞIRAN TARAF m_overlayMutex'i
// ZATEN TUTUYOR OLMALIDIR (kilitlemez/kilit açmaz). Bu sayede: iki thread'in
// mutasyon+render dizileri ASLA iç içe geçemez – biri tam bitmeden diğeri
// başlayamaz (aynı mutex için sırada bekler). Dolayısıyla hangi thread'in
// mutasyonu MUTEX SIRASINDA en son gelirse, o thread'in render'ı da HER
// ZAMAN en son (ve dolayısıyla ekranda kalıcı olan) render olur – araya
// başka bir şeyin girmesi YAPISAL OLARAK imkânsızdır. Eskiden ayrı bir
// "nesil sayacı" kontrolü kullanılıyordu; artık render çağrısının kendisi
// atomik hâle geldiği için o kontrol gereksizleşti (kaldırıldı), ama
// m_overlayGeneration sayacının kendisini basit bir tanılama/loglama
// aracı olarak bırakıyoruz.
void Application::RenderOverlayMapLocked()
{
    std::vector<TranslatedRegion> toRender;
    toRender.reserve(m_overlayMap.size());
    for (auto& [id, r] : m_overlayMap)
        toRender.push_back(r);
    Logger::DebugF("Overlay: {} bölge render'a gönderiliyor.", toRender.size());
    m_overlay->UpdateRegions(std::move(toRender));
}

void Application::FlushOverlayMap()
{
    std::lock_guard lock(m_overlayMutex);
    RenderOverlayMapLocked();
}

// ─── Hotkey handler ──────────────────────────────────────────────────────────
void Application::OnHotkey(const std::string& id)
{
    if      (id == "toggle_continuous")
    {
        if (m_mode == AppMode::ContinuousRunning) StopContinuous();
        else                                      StartContinuous();
    }
    else if (id == "one_shot")      TriggerOneShot();
    else if (id == "open_settings") OpenSettingsWindow();
    else if (id == "exit")          ::PostQuitMessage(0);
}

// ─── Tray handler ────────────────────────────────────────────────────────────
void Application::OnTrayCommand(TrayCommand cmd)
{
    switch (cmd)
    {
    case TrayCommand::StartContinuous: StartContinuous();    break;
    case TrayCommand::StopContinuous:  StopContinuous();     break;
    case TrayCommand::OneShot:         TriggerOneShot();     break;
    case TrayCommand::OpenSettings:    OpenSettingsWindow();  break;
    case TrayCommand::Exit:
        StopContinuous();
        ::PostQuitMessage(0);
        break;
    }
}

// ─── Ayarlar penceresi ────────────────────────────────────────────────────────
void Application::OpenSettingsWindow()
{
    if (!m_config) return;

    AppConfig before = m_config->Get();

    SettingsWindow::Show(m_hInstance, m_msgHwnd, before,
        [this, before](const AppConfig& updated)
        {
            m_config->Get() = updated;
            m_config->Save();
            Logger::Info("Ayarlar diske kaydedildi.");

            // ── Canlı uygulama (hot-reload) – yeniden başlatma GEREKMEZ ─────────
            // OverlayEngine: D3D cihazı/takas zinciri/DirectComposition'a
            // dokunulmadan yalnızca metin formatı ve fırçalar yenileniyor.
            if (m_overlay)
                m_overlay->UpdateConfig(updated.overlay);

            // OCREngine: kaynak dil değiştiyse OCR motorunu canlı olarak
            // yeniden oluştur (worker thread'e dokunulmaz).
            if (m_ocr && before.translation.srcLang != updated.translation.srcLang)
            {
                HRESULT hr = m_ocr->UpdateSourceLanguage(updated.translation.srcLang);
                if (FAILED(hr))
                    Logger::Error("OCREngine: kaynak dil güncellemesi başarısız oldu.");
            }

            // TranslationEngine: hedef dil/örnekleme parametreleri her
            // üretimde okunduğu için model yeniden yüklenmiyor; yalnızca
            // GGUF dosya adı değiştiyse model yeniden yükleniyor (birkaç
            // saniye sürebilir, ana thread'i bloklamaması için arka planda
            // çalıştırılıyor).
            if (m_translation)
            {
                bool modelChanged = before.translation.ggufFileName != updated.translation.ggufFileName;
                if (modelChanged)
                {
                    std::thread([this, updated]{
                        HRESULT hr = m_translation->UpdateConfig(updated.translation);
                        if (FAILED(hr))
                            Logger::Error("TranslationEngine: model güncellemesi başarısız oldu.");
                    }).detach();
                }
                else
                {
                    m_translation->UpdateConfig(updated.translation);
                }
            }
            // Kısayollar: değiştiyse HotkeyManager'a canlı olarak yeniden
            // kaydettir. TryRegister/RegisterAll, Windows'un o kombinasyonun
            // BAŞKA bir uygulama tarafından zaten kullanıldığını (Ayarlar
            // penceresindeki statik doğrulama bunu BİLEMEZ, çünkü bu ancak
            // gerçek RegisterHotKey çağrısı anında anlaşılır) bildirirse
            // kullanıcıyı uyarıyoruz – hotkey KAYITSIZ kalır ama uygulama
            // çalışmaya devam eder.
            if (m_hotkeys && before.hotkeys != updated.hotkeys)
            {
                bool allOk = m_hotkeys->RegisterAll(updated.hotkeys);
                if (!allOk)
                {
                    ::MessageBoxW(m_msgHwnd,
                        L"Bazı kısayollar kaydedilemedi (muhtemelen başka bir "
                        L"uygulama tarafından zaten kullanılıyor). Ayarlar "
                        L"penceresinden farklı bir kombinasyon seçmeyi deneyin.\n\n"
                        L"Ayrıntılar için log dosyasına bakabilirsiniz.",
                        L"Kısayol Kaydı Başarısız", MB_OK | MB_ICONWARNING);
                }
            }
        },
        // onClearCache: bkz. TranslationEngine::ClearCache yorumu ve
        // kCacheFormatVersion – kullanıcının "aynı kelimeleri kaydedip
        // onları mı çağırıyor?" sorusuna karşılık elle temizleme imkânı.
        [this]()
        {
            if (m_translation)
                m_translation->ClearCache();
            // Diskteki dosyayı da HEMEN sıfırlıyoruz (yalnızca bellekteki
            // m_cache'i temizlemek yeterli olurdu – zaten bir sonraki
            // SaveCache boş yazardı – ama kullanıcı temizledikten hemen
            // sonra uygulamayı normal kapanış DIŞINDA bir şekilde
            // sonlandırırsa (örn. görev yöneticisinden) SaveCache hiç
            // çalışmayabilir; dosyayı burada da sıfırlamak bunu garantiye
            // alır).
            if (m_translation)
                m_translation->SaveCache(m_translationCachePath);
        });
}

// ─── Message window ──────────────────────────────────────────────────────────
HWND Application::CreateMessageWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = L"ScreenLanceMsgWnd";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(0, L"ScreenLanceMsgWnd", L"",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, m_hInstance, this);
    return hwnd;
}

LRESULT CALLBACK Application::MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* app = reinterpret_cast<Application*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 1;
    }

    if (!app) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
    case WM_HOTKEY:
        app->m_hotkeys->OnHotkeyMessage(wp);
        return 0;
    case TrayIcon::WM_TRAY:
        app->m_tray->OnTrayMessage(lp);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
