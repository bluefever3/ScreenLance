#pragma once
#include "../Utils/Common.h"
#include "Config.h"

// Basit, tek sayfalık native Win32 Ayarlar penceresi.
//
// Neden WinUI3/XAML DEĞİL: Bu proje uçtan uca saf Win32 (message-loop tabanlı,
// App.xaml/Package.appxmanifest bootstrap'i yok). WinUI3 XAML Islands eklemek
// derleme zinciri (XAML compiler, codegen, Microsoft.UI.Xaml paketi) ve
// runtime bellek/CPU yükü açısından bu küçük ayar penceresi için orantısız
// ağır olurdu. Onun yerine standart Win32 ortak kontrolleri (BUTTON, EDIT,
// COMBOBOX, trackbar) kullanılıyor; bunlar işletim sisteminde zaten hazır,
// ek DLL/bağımlılık getirmiyor ve neredeyse anında açılıp kapanıyor.
//
// Windows 11 görünümü DWM API'leri ile sağlanıyor: yuvarlak köşeler
// (DWMWA_WINDOW_CORNER_PREFERENCE), sistem aydınlık/karanlık moduna uyum
// (DWMWA_USE_IMMERSIVE_DARK_MODE) ve Mica arka plan (DWMWA_SYSTEMBACKDROP_TYPE).
class SettingsWindow
{
public:
    // Modal olarak açar; kullanıcı "Kaydet"e basarsa onSave, güncellenmiş
    // AppConfig ile çağrılır (diske yazma ve engine'lerin yeniden
    // başlatılması çağıran tarafın sorumluluğundadır).
    // onClearCache: kullanıcı "Önbelleği Temizle" düğmesine basıp onayladığında
    // çağrılır (bkz. TranslationEngine::ClearCache) – opsiyoneldir, boşsa
    // düğme devre dışı bırakılır.
    static void Show(HINSTANCE hInstance, HWND owner, const AppConfig& current,
                      std::function<void(const AppConfig&)> onSave,
                      std::function<void()> onClearCache = nullptr);

private:
    struct HotkeyRow
    {
        HWND comboLabel{ nullptr }; // "Ctrl+F8" gösterir; kayıt sırasında "Basın..." / hata metnine döner
        HWND recordBtn { nullptr };
    };

    struct State
    {
        HINSTANCE hInstance{ nullptr };
        AppConfig config;                              // düzenlenmekte olan kopya
        std::function<void(const AppConfig&)> onSave;
        std::function<void()> onClearCache;

        // Child kontrol handle'ları
        HWND hSrcLangCombo  { nullptr }; // "Otomatik Algıla" + dil listesi
        HWND hDstLangCombo  { nullptr };
        HWND hQualityCombo  { nullptr };
        HWND hBlurCombo     { nullptr };
        HWND hOpacitySlider { nullptr };
        HWND hOpacityLabel  { nullptr };
        HWND hBgColorBtn    { nullptr };
        HWND hAreaModeCombo { nullptr };
        HWND hOneShotDurationEdit  { nullptr }; // kullanıcı süreyi doğrudan yazabilir
        HWND hOneShotDurationSpin  { nullptr }; // + yukarı/aşağı ok düğmeleri

        // Kısayol düzenleme: config.hotkeys ile AYNI SIRADA/UZUNLUKTA.
        std::vector<HotkeyRow> hotkeyRows;

        // Kaydırma (bkz. .cpp – çerçeve dışına taşmayı önlemek için).
        int contentHeight { 0 };     // gerçek içerik yüksekliği (piksel)
        int scrollPos     { 0 };
        bool scrollEnabled{ false };

        COLORREF bgColor{ RGB(0,0,0) };
        COLORREF customColors[16]{};
    };

    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static void CreateControls (HWND hwnd, State* st);
    static void LoadFromConfig (HWND hwnd, State* st);
    static bool SaveToConfig   (HWND hwnd, State* st); // false = doğrulama hatası
    static void ApplyWin11Style(HWND hwnd);
    static void UpdateColorButtons(State* st);
    static void UpdateOpacityLabel(State* st);

    // ── Kısayol yakalama ──────────────────────────────────────────────────
    // Kullanıcı bir "Değiştir" düğmesine bastığında, düşük seviyeli bir
    // klavye kancası (WH_KEYBOARD_LL) kurulur. Bu, standart diyalog klavye
    // işlemenin (Tab/Enter/Esc gibi tuşları diyalog yöneticisinin yutması)
    // ARKASINDAN dolaşıp HERHANGİ bir kombinasyonu (Ctrl+Shift+X gibi)
    // güvenilir şekilde yakalamamızı sağlar. Kanca GLOBAL bir Win32 API'si
    // olduğundan (HWND'ye değil çağıran THREAD'e bağlıdır), geri çağırma
    // sırasında hangi State/index'in kaydedildiğini bilmek için birkaç
    // dosya-kapsamlı statik değişken kullanılıyor (bkz. .cpp) – bu güvenli,
    // çünkü diyalog MODAL olduğundan aynı anda yalnızca bir tane açık olabilir.
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void StartRecording (HWND hwnd, State* st, int index);
    static void FinishRecording(bool success, const std::wstring& errorMsg = L"");
    static void UpdateHotkeyRowText(State* st, int index);

    // ── Kaydırma ───────────────────────────────────────────────────────────
    // İçerik, ekranın çalışma alanından (görev çubuğu hariç) uzun olursa
    // pencere artık ekranın dışına taşmıyor: pencere yüksekliği çalışma
    // alanına sığacak şekilde SINIRLANIYOR, fazla içerik dikey bir kaydırma
    // çubuğuyla (WM_VSCROLL / fare tekerleği) gezilebiliyor. Kaydırma,
    // ScrollWindowEx(SW_SCROLLCHILDREN) ile TÜM çocuk kontrolleri tek
    // seferde kaydırarak yapılır – her kontrolün pozisyonunu elle takip
    // etmeye gerek kalmaz.
    static void UpdateScroll(HWND hwnd, State* st, int newPos);

    // Kayıt oturumu için dosya-kapsamlı statik durum (bkz. yukarıdaki yorum).
    static HHOOK  s_kbHook;
    static State* s_recState;
    static int    s_recIndex;
    static HWND   s_recDialogHwnd;

    // Dil kodu <-> görünen isim (TranslationEngine::LangCodeToFullName ile tutarlı liste)
    static const std::vector<std::pair<std::wstring, std::wstring>>& LanguageList();

    // Kaynak dil listesi: başta "Otomatik Algıla" (kod = boş string) olmak
    // üzere LanguageList() ile aynı diller. OCREngine bu kodu görürse
    // OcrEngine::TryCreateFromUserProfileLanguages() ile otomatik seçim yapar.
    static const std::vector<std::pair<std::wstring, std::wstring>>& SourceLanguageList();

    // Kalite ön ayarları: {isim, maxTokens}. temperature/topP/topK Tencent'in
    // önerdiği değerlerde sabit tutuluyor (Translation/TranslationEngine.cpp).
    static const std::vector<std::pair<std::wstring, int>>& QualityPresets();
};
