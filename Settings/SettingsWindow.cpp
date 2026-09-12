#include "../Utils/pch.h"
#include "SettingsWindow.h"
#include "../Utils/Logger.h"
#include "../Hotkeys/HotkeyManager.h"
#include "../App/AppResource.h"
#include "../OCR/OCREngine.h"
#include <commdlg.h>
#include <commctrl.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

// Common Controls v6 (temalı/Windows 11 görünümlü BUTTON/EDIT/COMBOBOX vb.)
// .rc dosyası olmadan, linker'a manifest fragmanı gömerek etkinleştiriyoruz.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' "                      \
    "name='Microsoft.Windows.Common-Controls' "                \
    "version='6.0.0.0' processorArchitecture='*' "              \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

// ─── DWM sabitleri (bazı eski Windows SDK sürümlerinde eksik olabilir) ──────
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

// ─── Kontrol ID'leri ──────────────────────────────────────────────────────────
enum : int
{
    IDC_SRCLANG = 1001,
    IDC_DSTLANG,
    IDC_QUALITY,
    IDC_BLURMODE,
    IDC_OPACITY,
    IDC_OPACITY_LABEL,
    IDC_BGCOLOR_BTN,
    IDC_AREAMODE,
    IDC_CLEARCACHE_BTN,
    IDC_ONESHOT_DURATION,
    // Kısayol "Değiştir" düğmeleri: her hotkey için IDC_HOTKEY_BTN_BASE+index.
    // En fazla kMaxHotkeyRows kadar kısayol desteklenir (bugün 4 tane var,
    // gelecekte eklenirse diye pay bırakıldı).
    IDC_HOTKEY_BTN_BASE = 1100,
    IDC_INSTALL_LANGPACK_BTN = 1099,
};
static constexpr int kMaxHotkeyRows = 32;

// ─── Kısayol kaydı – dosya-kapsamlı statik durum ────────────────────────────
// Bkz. SettingsWindow.h'daki yorum: WH_KEYBOARD_LL global bir kancadır,
// geri çağırma imzası (LowLevelKeyboardProc) `this` taşımaz, bu yüzden aktif
// kayıt oturumunun durumu burada statik üyelerde tutuluyor. Diyalog MODAL
// olduğundan (DialogBoxIndirectParamW) aynı anda yalnızca bir örnek açık
// olabilir, dolayısıyla bu paylaşılan durum güvenlidir.
HHOOK               SettingsWindow::s_kbHook       = nullptr;
SettingsWindow::State* SettingsWindow::s_recState  = nullptr;
int                 SettingsWindow::s_recIndex     = -1;
HWND                SettingsWindow::s_recDialogHwnd= nullptr;


// ─── Dil / kalite listeleri ───────────────────────────────────────────────────
const std::vector<std::pair<std::wstring, std::wstring>>& SettingsWindow::LanguageList()
{
    // Hy-MT2'nin desteklediği diller arasından en yaygın olanlar.
    // Kod <-> tam isim eşlemesi Translation/TranslationEngine.cpp'deki
    // LangCodeToFullName() ile tutarlı tutulmalı.
    static const std::vector<std::pair<std::wstring, std::wstring>> kList = {
        { L"tr-TR", L"Türkçe" },      { L"en-US", L"İngilizce" },
        { L"zh-CN", L"Çince" },       { L"fr-FR", L"Fransızca" },
        { L"pt-PT", L"Portekizce" },  { L"es-ES", L"İspanyolca" },
        { L"ja-JP", L"Japonca" },     { L"ru-RU", L"Rusça" },
        { L"ar-SA", L"Arapça" },      { L"ko-KR", L"Korece" },
        { L"de-DE", L"Almanca" },     { L"it-IT", L"İtalyanca" },
        { L"nl-NL", L"Hollandaca" },  { L"pl-PL", L"Lehçe" },
    };
    return kList;
}

const std::vector<std::pair<std::wstring, std::wstring>>& SettingsWindow::SourceLanguageList()
{
    // Başta "Otomatik Algıla" (kod = boş string) olmak üzere LanguageList()
    // ile aynı diller. OCREngine bu boş kodu görürse
    // OcrEngine::TryCreateFromUserProfileLanguages() ile otomatik seçim yapar.
    static std::vector<std::pair<std::wstring, std::wstring>> kList;
    if (kList.empty())
    {
        kList.emplace_back(L"", L"Otomatik Algıla");
        for (auto& p : LanguageList()) kList.push_back(p);
    }
    return kList;
}

const std::vector<std::pair<std::wstring, int>>& SettingsWindow::QualityPresets()
{
    // temperature/topP/topK Tencent'in Hy-MT2 1.8B/7B için önerdiği
    // sabit değerlerde kalır (0.7 / 0.6 / 20); yalnızca üretilecek maksimum
    // token sayısı (dolayısıyla hız/uzunluk) değişir.
    static const std::vector<std::pair<std::wstring, int>> kList = {
        { L"Hızlı (kısa metinler için)",        256  },
        { L"Dengeli (önerilen)",                 512  },
        { L"Kaliteli (uzun metinler, daha yavaş)", 1024 },
    };
    return kList;
}

// ─── Yardımcılar ──────────────────────────────────────────────────────────────
static std::wstring FormatHotkey(const HotkeyDef& d)
{
    std::wstring s;
    if (d.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (d.modifiers & MOD_ALT)     s += L"Alt+";
    if (d.modifiers & MOD_SHIFT)   s += L"Shift+";
    if (d.modifiers & MOD_WIN)     s += L"Win+";

    if (d.vk >= VK_F1 && d.vk <= VK_F24)
        s += L"F" + std::to_wstring(d.vk - VK_F1 + 1);
    else
    {
        wchar_t name[64]{};
        LONG scan = static_cast<LONG>(::MapVirtualKeyW(d.vk, MAPVK_VK_TO_VSC)) << 16;
        if (::GetKeyNameTextW(scan, name, 64) > 0)
            s += name;
        else
            s += L"?";
    }
    return s;
}

static std::wstring FriendlyHotkeyId(const std::string& id)
{
    if (id == "toggle_continuous") return L"Sürekli modu aç/kapat";
    if (id == "one_shot")          return L"Tek seferlik çeviri";
    if (id == "open_settings")     return L"Ayarları aç";
    if (id == "exit")              return L"Çıkış";
    return std::wstring(id.begin(), id.end());
}

// ─── Kısayol yakalama ──────────────────────────────────────────────────────
void SettingsWindow::UpdateHotkeyRowText(State* st, int index)
{
    if (index < 0 || index >= static_cast<int>(st->config.hotkeys.size())) return;
    HWND label = st->hotkeyRows[index].comboLabel;
    if (!label) return;
    ::SetWindowTextW(label, FormatHotkey(st->config.hotkeys[index]).c_str());
}

// Aktif kayıt oturumunu (varsa) temizler: kancayı kaldırır, düğme/etiket
// metinlerini eski hâline döndürür, TÜM "Değiştir" düğmelerini yeniden
// etkinleştirir. success=false ise ve errorMsg doluysa kullanıcıya neden
// bir MessageBoxW ile gösterilir (kanca kaldırıldıktan SONRA gösterilir –
// aksi hâlde mesaj kutusuyla etkileşim (Enter/Esc) kancamız tarafından
// yutulurdu).
void SettingsWindow::FinishRecording(bool success, const std::wstring& errorMsg)
{
    if (s_kbHook) { ::UnhookWindowsHookEx(s_kbHook); s_kbHook = nullptr; }

    State* st   = s_recState;
    int    idx  = s_recIndex;
    HWND   hwnd = s_recDialogHwnd;

    s_recState      = nullptr;
    s_recIndex      = -1;
    s_recDialogHwnd = nullptr;

    if (!st) return;

    // Etiketi (kaydedilen yeni kombinasyon YA DA vazgeçilmişse eski hâli)
    // güncelle ve tüm "Değiştir" düğmelerini tekrar etkinleştir.
    UpdateHotkeyRowText(st, idx);
    for (auto& row : st->hotkeyRows)
        if (row.recordBtn)
            ::EnableWindow(row.recordBtn, TRUE);
    if (idx >= 0 && idx < static_cast<int>(st->hotkeyRows.size()) && st->hotkeyRows[idx].recordBtn)
        ::SetWindowTextW(st->hotkeyRows[idx].recordBtn, L"Değiştir");

    if (!success && !errorMsg.empty() && hwnd)
        ::MessageBoxW(hwnd, errorMsg.c_str(), L"Geçersiz Kısayol", MB_OK | MB_ICONWARNING);
}

void SettingsWindow::StartRecording(HWND hwnd, State* st, int index)
{
    if (s_recIndex != -1) return; // zaten bir kayıt sürüyor
    if (index < 0 || index >= static_cast<int>(st->hotkeyRows.size())) return;

    s_recState      = st;
    s_recIndex      = index;
    s_recDialogHwnd = hwnd;

    ::SetWindowTextW(st->hotkeyRows[index].comboLabel, L"Bir tuş kombinasyonuna basın...");
    ::SetWindowTextW(st->hotkeyRows[index].recordBtn,  L"İptal (ESC)");
    for (auto& row : st->hotkeyRows)
        if (row.recordBtn && row.recordBtn != st->hotkeyRows[index].recordBtn)
            ::EnableWindow(row.recordBtn, FALSE);

    // Düşük seviyeli klavye kancası: WH_KEYBOARD_LL, Windows tarafından
    // HER ZAMAN sistem geneli (tüm işlemler için) kurulur – dwThreadId
    // parametresi bu kanca türü için YOK SAYILIR ve 0 geçilmesi ZORUNLUDUR.
    // Geri çağırma, kancayı KURAN thread'in mesaj kuyruğunda çalışır; bu da
    // modal diyaloğumuzun kendi mesaj döngüsü olduğu için güvenilir şekilde
    // tetiklenir. Kayıt bittiğinde (FinishRecording) HEMEN kaldırıyoruz ki
    // sistem genelindeki tüm klavye girdisini gereksiz yere dinlemeye devam
    // etmeyelim.
    s_kbHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                    ::GetModuleHandleW(nullptr), 0);
    if (!s_kbHook)
    {
        Logger::Error("SettingsWindow: klavye kancası kurulamadı, kısayol kaydı iptal edildi.");
        FinishRecording(false, L"Klavye dinleyicisi kurulamadı, lütfen tekrar deneyin.");
    }
}

LRESULT CALLBACK SettingsWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_recIndex != -1 &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = kb->vkCode;

        // Salt değiştirici tuşlar (Ctrl/Alt/Shift/Win) tek başına henüz bir
        // kombinasyon OLUŞTURMAZ – asıl "gerçek" tuşu bekliyoruz. Bunları
        // yutmadan (return CallNextHookEx) geçiyoruz ki normal davranışları
        // (örn. Alt tek başına menü çubuğunu tetiklemesi) bozulmasın; zaten
        // aşağıdaki modifiers hesaplaması GetAsyncKeyState ile her an
        // güncel durumu okuyacak.
        bool isPureModifier =
            (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
             vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
             vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
             vk == VK_LWIN    || vk == VK_RWIN);

        if (vk == VK_ESCAPE)
        {
            // ESC tek başına: kayıttan vazgeç. Kancayı BURADA (callback
            // içinde) kaldırmak güvenlidir; Windows bunu destekler.
            FinishRecording(false);
            return 1; // ESC'nin diyaloğu KAPATMASINI (IDCANCEL) engelle
        }

        if (!isPureModifier)
        {
            UINT mod = 0;
            if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (::GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
            if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) || (::GetAsyncKeyState(VK_RWIN) & 0x8000))
                mod |= MOD_WIN;

            State* st  = s_recState;
            int    idx = s_recIndex;

            if (mod == 0)
            {
                // Değiştirici YOK: yalnızca normal yazarken kazara
                // tetiklenecek bir global kısayol yaratmamak için EN AZ bir
                // değiştirici (Ctrl/Alt/Shift/Win) zorunlu tutuyoruz. Kayıt
                // modunda kalıp kullanıcıya tekrar denemesi için izin
                // veriyoruz (kancayı KALDIRMIYORUZ).
                if (st) ::SetWindowTextW(st->hotkeyRows[idx].comboLabel,
                    L"En az bir değiştirici (Ctrl/Alt/Shift/Win) gerekli...");
                return 1;
            }

            std::wstring reason;
            if (!HotkeyManager::ValidateCombination(mod, vk, reason))
            {
                // Windows'un ayırdığı bir kombinasyon (bkz.
                // HotkeyManager::IsReserved) – KAYIT MODUNDAN ÇIK ve
                // kullanıcıyı bilgilendir, tekrar "Değiştir"e basıp
                // denemesi gerekir (sürekli aynı hatayı vermeye devam
                // etmemek için modda kalmak yerine çıkmayı tercih ettik).
                FinishRecording(false, L"Bu kombinasyon Windows tarafından "
                    L"kullanılıyor, lütfen başka bir kombinasyon seçin.\n\n" + reason);
                return 1;
            }

            // Uygulamanın KENDİ diğer kısayotlarıyla çakışma kontrolü –
            // HotkeyManager::IsReserved bunu bilemez, bu YALNIZCA bizim
            // config.hotkeys listemize özgü bir kural.
            if (st)
            {
                for (int i = 0; i < static_cast<int>(st->config.hotkeys.size()); ++i)
                {
                    if (i == idx) continue;
                    auto& other = st->config.hotkeys[i];
                    if (other.modifiers == mod && other.vk == vk)
                    {
                        std::wstring msg = L"Bu kombinasyon zaten '" +
                            FriendlyHotkeyId(other.id) + L"' için kullanılıyor.";
                        FinishRecording(false, msg);
                        return 1;
                    }
                }

                // Başarılı: yeni kombinasyonu kaydet.
                st->config.hotkeys[idx].modifiers = mod;
                st->config.hotkeys[idx].vk        = vk;
                Logger::InfoF("SettingsWindow: kısayol '{}' güncellendi.", st->config.hotkeys[idx].id);
            }
            FinishRecording(true);
            return 1;
        }
    }
    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// DWORD boundary'e hizala
static void AlignDword(std::vector<BYTE>& data)
{
    while (data.size() % 4 != 0) data.push_back(0);
}
static void Push16(std::vector<BYTE>& data, WORD w)
{
    data.push_back(static_cast<BYTE>(w & 0xFF));
    data.push_back(static_cast<BYTE>((w >> 8) & 0xFF));
}
static void Push32(std::vector<BYTE>& data, DWORD d)
{
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<BYTE>((d >> (i * 8)) & 0xFF));
}
static void PushStr(std::vector<BYTE>& data, const wchar_t* s)
{
    while (*s) { Push16(data, static_cast<WORD>(*s)); ++s; }
    Push16(data, 0);
}

// Boş bir DLGTEMPLATE üretir (item'lar WM_INITDIALOG içinde CreateWindowExW
// ile elle eklenecek). Gerçek pencere boyutu da WM_INITDIALOG'da piksel
// cinsinden yeniden ayarlanıyor; buradaki cx/cy sadece ilk yaklaşık değer.
static std::vector<BYTE> BuildEmptyDialogTemplate(const wchar_t* title)
{
    std::vector<BYTE> data;
    AlignDword(data);

    DWORD style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    Push32(data, style);
    Push32(data, 0);           // dwExtendedStyle
    Push16(data, 0);           // cdit (item sayısı = 0)
    Push16(data, 0); Push16(data, 0); // x, y
    Push16(data, 300); Push16(data, 250); // cx, cy (dialog unit, sonradan piksel olarak yeniden boyutlanacak)
    Push16(data, 0);           // menu = yok
    Push16(data, 0);           // window class = varsayılan (#32770)
    PushStr(data, title);
    Push16(data, 9);           // point size
    PushStr(data, L"Segoe UI");
    AlignDword(data);
    return data;
}

// ─── Windows 11 stili (DWM) ───────────────────────────────────────────────────
void SettingsWindow::ApplyWin11Style(HWND hwnd)
{
    // Sistem karanlık modda mı?
    BOOL dark = FALSE;
    DWORD val = 1, size = sizeof(val);
    HKEY hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (::RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
        {
            dark = (val == 0);
        }
        ::RegCloseKey(hKey);
    }

    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    DWORD corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    DWORD backdrop = DWMSBT_MAINWINDOW; // Mica
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
}

// ─── Kontrol oluşturma ────────────────────────────────────────────────────────
static HWND MkStatic(HWND parent, HINSTANCE hi, const wchar_t* text, int x, int y, int w, int h)
{
    return ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, nullptr, hi, nullptr);
}
static HWND MkGroup(HWND parent, HINSTANCE hi, const wchar_t* text, int x, int y, int w, int h)
{
    return ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, parent, nullptr, hi, nullptr);
}
static HWND MkCombo(HWND parent, HINSTANCE hi, int id, int x, int y, int w)
{
    return ::CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
}
static HWND MkButton(HWND parent, HINSTANCE hi, int id, const wchar_t* text, int x, int y, int w, int h)
{
    return ::CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
}

void SettingsWindow::CreateControls(HWND hwnd, State* st)
{
    HINSTANCE hi = st->hInstance;
    const int marginX = 20;
    const int groupW  = 420;
    int y = 16;

    // ── Çevrilecek Alan ──────────────────────────────────────────────────────
    // Tek seferlik çeviri VE sürekli çeviri, bu ayara göre davranır (bkz.
    // Application::TriggerOneShot / StartContinuous): "El ile Seçim"de her
    // seferinde fareyle bir bölge seçilir; "Tam Ekran"da hiç seçim
    // istenmeden doğrudan yapılandırılmış monitör(ler)in tamamı kullanılır.
    MkGroup(hwnd, hi, L"Çevrilecek Alan", marginX, y, groupW, 66);
    MkStatic(hwnd, hi, L"Alan seçimi:", marginX + 16, y + 30, 100, 20);
    st->hAreaModeCombo = MkCombo(hwnd, hi, IDC_AREAMODE, marginX + 130, y + 27, 260);
    ::SendMessageW(st->hAreaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"El ile Seçim (her seferinde fareyle seç)"));
    ::SendMessageW(st->hAreaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tam Ekran (tüm monitörler, otomatik)"));
    ::SendMessageW(st->hAreaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Aktif Monitör (yalnızca imlecin olduğu ekran)"));
    y += 66 + 14;

    // ── Dil ─────────────────────────────────────────────────────────────────
    MkGroup(hwnd, hi, L"Dil", marginX, y, groupW, 122);
    MkStatic(hwnd, hi, L"Kaynak dil:", marginX + 16, y + 28, 100, 20);
    st->hSrcLangCombo = MkCombo(hwnd, hi, IDC_SRCLANG, marginX + 130, y + 25, 260);
    for (auto& [code, name] : SourceLanguageList())
        ::SendMessageW(st->hSrcLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    MkStatic(hwnd, hi, L"Hedef dil:", marginX + 16, y + 58, 100, 20);
    st->hDstLangCombo = MkCombo(hwnd, hi, IDC_DSTLANG, marginX + 130, y + 55, 180);
    for (auto& [code, name] : LanguageList())
        ::SendMessageW(st->hDstLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    // Windows OCR, seçilen kaynak dil için "Yazı Tanıma" dil paketi kurulu
    // DEĞİLSE boş sonuç döner (uygulama çalışıyormuş gibi görünür ama hiçbir
    // şey algılamaz) – bu, kullanıcıların en çok kafa karıştıran
    // durumlarından biri. Bu düğme, Ayarlar > Saat ve Dil menüsünde manuel
    // aramaya gerek kalmadan, Windows'un kendi indirme/onay penceresini
    // AÇARAK dil paketini tek tıkla kurmayı sağlar (bkz.
    // OCREngine::InstallLanguagePackAsync).
    MkButton(hwnd, hi, IDC_INSTALL_LANGPACK_BTN, L"Dil Paketi Ayar Sayfasını Aç...",
             marginX + 130, y + 88, 260, 26);
    y += 122 + 14;

    // ── Çeviri Kalitesi ─────────────────────────────────────────────────────
    // NOT: Mod combobox'ı ile "Önbelleği Temizle" düğmesi önceden AYNI
    // satırda yan yana konumlandırılmıştı; toplam genişlikleri (130+180+10+150)
    // grup kutusunun genişliğini (groupW=420) aşıyordu, bu yüzden düğme
    // çerçevenin dışına taşıyordu. Düzeltme: düğmeyi kendi satırına alıp
    // grubu buna göre yükselttik.
    MkGroup(hwnd, hi, L"Çeviri Kalitesi", marginX, y, groupW, 96);
    MkStatic(hwnd, hi, L"Mod:", marginX + 16, y + 30, 100, 20);
    st->hQualityCombo = MkCombo(hwnd, hi, IDC_QUALITY, marginX + 130, y + 27, 260);
    for (auto& [name, tokens] : QualityPresets())
        ::SendMessageW(st->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    // Çeviri önbelleği: aynı metin bir daha ASLA modele gönderilmeden bu
    // önbellekten döner (bkz. TranslationEngine::CacheLookup). Prompt veya
    // uzunluk mantığı iyileştirildiğinde DAHA ÖNCE kaydedilmiş eski/kötü
    // sonuçlar otomatik olarak atılır (bkz. kCacheFormatVersion), ama
    // kullanıcı yine de belirli kelimelerin YENİDEN çevrilmesini istediğinde
    // (örn. dil/model ayarını değiştirip aynı ekranı test ederken) elle
    // temizleyebilsin diye bu düğme var.
    MkButton(hwnd, hi, IDC_CLEARCACHE_BTN, L"Önbelleği Temizle",
             marginX + 130, y + 60, 180, 26);
    y += 96 + 14;

    // ── Tek Seferlik Çeviri Süresi ───────────────────────────────────────────
    // Kullanıcı isteği: tek seferlik çevirinin ekranda ne kadar kalacağı
    // (önceden sabit 8 saniyeydi) artık yapılandırılabilir. Klasik bir
    // EDIT+UPDOWN ("buddy window") kombinasyonu kullanıyoruz – bu, hem
    // kullanıcının rakamı DOĞRUDAN KENDİSİNİN yazabilmesini (EDIT alanına
    // tıklayıp yazması yeterli) hem de yukarı/aşağı oklarla hızlıca
    // artırıp azaltabilmesini sağlıyor. UDS_SETBUDDYINT sayesinde UpDown
    // kontrolü EDIT'in metnini otomatik senkronize eder, ayrıca aralığı
    // (1–60) UpDown'ın kendisi de sınırlar (SaveToConfig'te ayrıca bir daha
    // clamp ediyoruz – kullanıcı EDIT'e elle "9999" gibi bir şey yazıp
    // Tab'lasa bile UpDown bunu anında 60'a çeker, ama çift kontrol zarar
    // vermez).
    MkGroup(hwnd, hi, L"Tek Seferlik Çeviri Süresi", marginX, y, groupW, 66);
    MkStatic(hwnd, hi, L"Ekranda kalma süresi:", marginX + 16, y + 30, 160, 20);
    st->hOneShotDurationEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
        marginX + 180, y + 27, 50, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ONESHOT_DURATION)), hi, nullptr);
    st->hOneShotDurationSpin = ::CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
        0, 0, 0, 0, hwnd, nullptr, hi, nullptr);
    ::SendMessageW(st->hOneShotDurationSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hOneShotDurationEdit), 0);
    ::SendMessageW(st->hOneShotDurationSpin, UDM_SETRANGE32, 1, 60);
    MkStatic(hwnd, hi, L"saniye (1-60)", marginX + 180 + 50 + 20, y + 30, 120, 20);
    y += 66 + 14;

    // ── Overlay Görünümü ────────────────────────────────────────────────────
    // NOT: Yazı boyutu ve rengi artık ekrandaki orijinal metinden otomatik
    // algılanıyor (bkz. OCREngine::EstimateRegionColors), bu yüzden burada
    // manuel bir "yazı boyutu/rengi" kontrolü YOK. Yalnızca kutunun arka planı
    // (algılama başarısız olursa kullanılan yedek renk) ayarlanabilir.
    MkGroup(hwnd, hi, L"Çeviri Kutusu Görünümü", marginX, y, groupW, 130);
    int gy = y + 26;

    MkStatic(hwnd, hi, L"Yazı boyutu ve rengi görüntüden otomatik algılanır.",
              marginX + 16, gy, groupW - 32, 20);
    gy += 30;

    MkStatic(hwnd, hi, L"Bulanıklaştırma:", marginX + 16, gy, 110, 20);
    st->hBlurCombo = MkCombo(hwnd, hi, IDC_BLURMODE, marginX + 130, gy - 3, 260);
    ::SendMessageW(st->hBlurCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bulanıklaştır"));
    ::SendMessageW(st->hBlurCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ortalama renk (en hafif)"));
    ::SendMessageW(st->hBlurCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Yarı saydam"));
    gy += 34;

    st->hBgColorBtn = MkButton(hwnd, hi, IDC_BGCOLOR_BTN, L"Yedek Arka Plan Rengi...", marginX + 16, gy, 220, 26);
    gy += 36;

    MkStatic(hwnd, hi, L"Arka plan opaklığı:", marginX + 16, gy, 130, 20);
    st->hOpacitySlider = ::CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        marginX + 150, gy - 4, 180, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPACITY)), hi, nullptr);
    ::SendMessageW(st->hOpacitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    ::SendMessageW(st->hOpacitySlider, TBM_SETTICFREQ, 10, 0);
    st->hOpacityLabel = MkStatic(hwnd, hi, L"70%", marginX + 340, gy, 50, 20);

    y += 130 + 14;

    // ── Kısayollar (düzenlenebilir) ─────────────────────────────────────────
    // Her satır: etiket (dostça isim) + mevcut kombinasyon + "Değiştir"
    // düğmesi. Düğmeye basınca StartRecording() bir WH_KEYBOARD_LL kancası
    // kurar (bkz. yukarıdaki yorumlar) ve kullanıcının bastığı kombinasyonu
    // hem Windows'un ayırdığı kombinasyonlara (HotkeyManager::IsReserved)
    // hem de uygulamanın DİĞER kısayotlarına karşı doğrular.
    int rowH = 30;
    int hotkeyH = 26 + static_cast<int>(st->config.hotkeys.size()) * rowH + 10;
    MkGroup(hwnd, hi, L"Kısayollar", marginX, y, groupW, hotkeyH);
    int hy = y + 26;
    st->hotkeyRows.resize(st->config.hotkeys.size());
    for (int i = 0; i < static_cast<int>(st->config.hotkeys.size()); ++i)
    {
        auto& hk = st->config.hotkeys[i];
        MkStatic(hwnd, hi, FriendlyHotkeyId(hk.id).c_str(), marginX + 16, hy + 4, 170, 20);

        HWND comboLabel = MkStatic(hwnd, hi, FormatHotkey(hk).c_str(),
                                    marginX + 16 + 175, hy + 4, 100, 20);

        int btnId = IDC_HOTKEY_BTN_BASE + i;
        HWND recordBtn = MkButton(hwnd, hi, btnId, L"Değiştir",
                                   marginX + 16 + 175 + 105, hy, 90, 24);

        st->hotkeyRows[i] = { comboLabel, recordBtn };
        hy += rowH;
    }
    y += hotkeyH + 20;

    // ── Butonlar ─────────────────────────────────────────────────────────────
    MkButton(hwnd, hi, IDCANCEL, L"İptal",  marginX + groupW - 180, y, 80, 30);
    MkButton(hwnd, hi, IDOK,     L"Kaydet", marginX + groupW - 90,  y, 90, 30);
    y += 30 + 16;

    // Pencereyi client alanına göre kesin boyuta getir ve ortala.
    //
    // KRİTİK HATA DÜZELTMESİ (çerçeve dışına taşma): önceden pencere HER
    // ZAMAN tüm içeriğe (y) TAM OTURACAK şekilde boyutlandırılıp ekranın
    // ORTASINA yerleştiriliyordu – ekranın çalışma alanından (görev çubuğu
    // hariç kullanılabilir alan) daha uzun bir içerik oluştuğunda (örn. çok
    // sayıda kısayol satırı eklenince), pencere ekranın ÜSTÜNDEN/ALTINDAN
    // taşıyor, kullanıcı "Kaydet" düğmesine bile erişemeyebiliyordu. Şimdi:
    // pencere yüksekliği ÇALIŞMA ALANINA sığacak şekilde SINIRLANIYOR, sığmayan
    // fazla içerik ScrollWindowEx(SW_SCROLLCHILDREN) tabanlı dikey bir
    // kaydırma çubuğuyla gezilebiliyor (bkz. UpdateScroll).
    st->contentHeight = y;

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int workH = work.bottom - work.top;
    int workW = work.right  - work.left;

    constexpr int kScreenMargin = 40; // çalışma alanının kenarlarına yapışmasın
    int maxClientH = (std::max)(200, workH - kScreenMargin);

    int clientW = marginX * 2 + groupW;
    int clientH = (std::min)(st->contentHeight, maxClientH);
    st->scrollEnabled = (st->contentHeight > maxClientH);

    RECT rc{ 0, 0, clientW, clientH };
    DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
    if (st->scrollEnabled)
        style |= WS_VSCROLL;
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
    ::AdjustWindowRect(&rc, style, FALSE);

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    // Ortalamayı ÇALIŞMA ALANINA göre yap (SM_CXSCREEN/SM_CYSCREEN DEĞİL) –
    // aksi hâlde görev çubuğu hesaba katılmayıp pencere yine kısmen onun
    // altında/ekran dışında kalabilirdi.
    int px = work.left + ((workW - winW) / 2);
    int py = work.top  + ((workH - winH) / 2);
    ::SetWindowPos(hwnd, nullptr, px, py, winW, winH,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (st->scrollEnabled)
    {
        SCROLLINFO si{ sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin  = 0;
        si.nMax  = st->contentHeight - 1;
        si.nPage = static_cast<UINT>(clientH);
        si.nPos  = 0;
        ::SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    }

    // Tüm kontrollere sistem fontunu uygula
    HFONT hFont = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        static HFONT s_uiFont = ::CreateFontIndirectW(&ncm.lfMessageFont);
        if (s_uiFont) hFont = s_uiFont;
    }
    ::EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(hFont));
}

// ─── Kaydırma ─────────────────────────────────────────────────────────────────
void SettingsWindow::UpdateScroll(HWND hwnd, State* st, int newPos)
{
    RECT client{};
    ::GetClientRect(hwnd, &client);
    int pageH = client.bottom - client.top;

    int maxPos = (std::max)(0, st->contentHeight - pageH);
    newPos = std::clamp(newPos, 0, maxPos);
    if (newPos == st->scrollPos) return;

    int delta = st->scrollPos - newPos; // pozitifse içerik AŞAĞI kayar (yukarı scroll)
    st->scrollPos = newPos;

    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_POS;
    si.nPos  = newPos;
    ::SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    // TÜM çocuk kontrolleri tek seferde kaydır – her birinin pozisyonunu
    // elle takip/güncellemeye gerek kalmaz.
    ::ScrollWindowEx(hwnd, 0, delta, nullptr, nullptr, nullptr, nullptr,
                      SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    ::UpdateWindow(hwnd);
}

// ─── Config -> UI ─────────────────────────────────────────────────────────────
void SettingsWindow::LoadFromConfig(HWND /*hwnd*/, State* st)
{
    const auto& cfg = st->config;

    // Çevrilecek alan
    ::SendMessageW(st->hAreaModeCombo, CB_SETCURSEL, static_cast<int>(cfg.capture.areaMode), 0);

    // Kaynak dil ("Otomatik Algıla" listenin ilk elemanı, kod = boş string)
    int srcIdx = 0, si = 0;
    for (auto& [code, name] : SourceLanguageList())
    {
        if (code == cfg.translation.srcLang) { srcIdx = si; break; }
        ++si;
    }
    ::SendMessageW(st->hSrcLangCombo, CB_SETCURSEL, srcIdx, 0);

    // Hedef dil
    int langIdx = 0, i = 0;
    for (auto& [code, name] : LanguageList())
    {
        if (code == cfg.translation.dstLang) { langIdx = i; break; }
        ++i;
    }
    ::SendMessageW(st->hDstLangCombo, CB_SETCURSEL, langIdx, 0);

    // Kalite (maxTokens'a en yakın ön ayar)
    int qIdx = 1; // varsayılan: Dengeli
    int bestDiff = INT_MAX, qi = 0;
    for (auto& [name, tokens] : QualityPresets())
    {
        int diff = tokens - cfg.translation.maxTokens;
        if (diff < 0) diff = -diff;
        if (diff < bestDiff) { bestDiff = diff; qIdx = qi; }
        ++qi;
    }
    ::SendMessageW(st->hQualityCombo, CB_SETCURSEL, qIdx, 0);

    // Overlay (yalnızca arka plan – yazı boyutu/rengi otomatik algılanıyor)
    ::SendMessageW(st->hBlurCombo, CB_SETCURSEL, static_cast<int>(cfg.overlay.blurMode), 0);

    st->bgColor = cfg.overlay.bgColor;
    UpdateColorButtons(st);

    int opacityPct = static_cast<int>(cfg.overlay.bgOpacity * 100.0f + 0.5f);
    ::SendMessageW(st->hOpacitySlider, TBM_SETPOS, TRUE, opacityPct);
    UpdateOpacityLabel(st);

    // Tek seferlik çeviri süresi
    ::SetWindowTextW(st->hOneShotDurationEdit,
        std::to_wstring(std::clamp(cfg.overlay.oneShotDisplaySeconds, 1, 60)).c_str());
}

void SettingsWindow::UpdateColorButtons(State* st)
{
    // Owner-draw renk önizleme kutusu yerine, basitlik için seçili rengi
    // buton metninde RGB olarak gösteriyoruz — ek çizim kodu gerektirmez,
    // erişilebilir (screen reader okuyabilir) ve az kaynak tüketir.
    if (!st->hBgColorBtn) return;
    wchar_t buf[64];
    swprintf_s(buf, L"Yedek Arka Plan Rengi (%d,%d,%d)",
               GetRValue(st->bgColor), GetGValue(st->bgColor), GetBValue(st->bgColor));
    ::SetWindowTextW(st->hBgColorBtn, buf);
}

void SettingsWindow::UpdateOpacityLabel(State* st)
{
    int pos = static_cast<int>(::SendMessageW(st->hOpacitySlider, TBM_GETPOS, 0, 0));
    wchar_t buf[16];
    swprintf_s(buf, L"%%%d", pos);
    ::SetWindowTextW(st->hOpacityLabel, buf);
}

// ─── UI -> Config ─────────────────────────────────────────────────────────────
bool SettingsWindow::SaveToConfig(HWND hwnd, State* st)
{
    // Çevrilecek alan
    int areaIdx = static_cast<int>(::SendMessageW(st->hAreaModeCombo, CB_GETCURSEL, 0, 0));
    if (areaIdx >= 0 && areaIdx <= 2)
        st->config.capture.areaMode = static_cast<CaptureAreaMode>(areaIdx);

    // Kaynak dil
    int srcIdx = static_cast<int>(::SendMessageW(st->hSrcLangCombo, CB_GETCURSEL, 0, 0));
    if (srcIdx >= 0 && srcIdx < static_cast<int>(SourceLanguageList().size()))
        st->config.translation.srcLang = SourceLanguageList()[srcIdx].first;

    // Hedef dil
    int langIdx = static_cast<int>(::SendMessageW(st->hDstLangCombo, CB_GETCURSEL, 0, 0));
    if (langIdx >= 0 && langIdx < static_cast<int>(LanguageList().size()))
        st->config.translation.dstLang = LanguageList()[langIdx].first;

    // Kalite -> maxTokens
    int qIdx = static_cast<int>(::SendMessageW(st->hQualityCombo, CB_GETCURSEL, 0, 0));
    if (qIdx >= 0 && qIdx < static_cast<int>(QualityPresets().size()))
        st->config.translation.maxTokens = QualityPresets()[qIdx].second;

    // Overlay (yalnızca arka plan – yazı boyutu/rengi/kalınlık/anahat artık
    // otomatik algılandığı için buradan hiç değiştirilmiyor, önceki
    // değerleri korunuyor)
    int blurIdx = static_cast<int>(::SendMessageW(st->hBlurCombo, CB_GETCURSEL, 0, 0));
    if (blurIdx >= 0 && blurIdx <= 2)
        st->config.overlay.blurMode = static_cast<BlurMode>(blurIdx);

    st->config.overlay.bgColor = st->bgColor;

    int opacityPct = static_cast<int>(::SendMessageW(st->hOpacitySlider, TBM_GETPOS, 0, 0));
    st->config.overlay.bgOpacity = opacityPct / 100.0f;

    // Tek seferlik çeviri süresi: EDIT'ten oku, güvenlik için burada da
    // clamp et (UpDown zaten sınırlıyor ama EDIT'e programatik/IME yoluyla
    // aralık dışı bir değer girme ihtimaline karşı ikinci bir güvenlik ağı).
    BOOL ok = FALSE;
    int seconds = static_cast<int>(::GetDlgItemInt(hwnd, IDC_ONESHOT_DURATION, &ok, FALSE));
    if (!ok || seconds <= 0) seconds = st->config.overlay.oneShotDisplaySeconds; // geçersizse eskisini koru
    st->config.overlay.oneShotDisplaySeconds = std::clamp(seconds, 1, 60);

    return true;
}

// ─── Dialog procedure ─────────────────────────────────────────────────────────
INT_PTR CALLBACK SettingsWindow::DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* st = reinterpret_cast<State*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        // Diyalog kutuları .exe'nin kendi ikonunu OTOMATİK almaz (yalnızca
        // Explorer/görev çubuğu/Alt+Tab .rc'deki ikonu kullanır) - küçük
        // bir tutarlılık dokunuşu olarak burada da aynı ikonu (bkz.
        // App/AppResources.rc) elle ayarlıyoruz.
        HICON hIconBig   = reinterpret_cast<HICON>(::LoadImageW(st->hInstance,
            MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        HICON hIconSmall = reinterpret_cast<HICON>(::LoadImageW(st->hInstance,
            MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (hIconBig)   ::SendMessageW(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIconBig));
        if (hIconSmall) ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));

        ApplyWin11Style(hwnd);
        CreateControls(hwnd, st);
        LoadFromConfig(hwnd, st);
        return TRUE;
    }

    case WM_HSCROLL:
    {
        auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && reinterpret_cast<HWND>(lp) == st->hOpacitySlider)
            UpdateOpacityLabel(st);
        return TRUE;
    }

    case WM_COMMAND:
    {
        auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!st) break;

        switch (LOWORD(wp))
        {
        case IDC_BGCOLOR_BTN:
        {
            CHOOSECOLORW cc{ sizeof(cc) };
            cc.hwndOwner   = hwnd;
            cc.lpCustColors = st->customColors;
            cc.rgbResult   = st->bgColor;
            cc.Flags       = CC_FULLOPEN | CC_RGBINIT;
            if (::ChooseColorW(&cc))
            {
                st->bgColor = cc.rgbResult;
                UpdateColorButtons(st);
            }
            return TRUE;
        }

        case IDC_CLEARCACHE_BTN:
        {
            // Yıkıcı bir işlem (diskteki tüm çeviri geçmişini siler), bu
            // yüzden önce onay istiyoruz.
            int r = ::MessageBoxW(hwnd,
                L"Kaydedilmiş TÜM çeviriler silinecek ve bundan sonra ilk "
                L"karşılaşıldıklarında yeniden çevrilecekler. Devam edilsin mi?",
                L"Çeviri Önbelleğini Temizle", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES && st->onClearCache)
            {
                st->onClearCache();
                Logger::Info("SettingsWindow: çeviri önbelleği kullanıcı isteğiyle temizlendi.");
                ::MessageBoxW(hwnd, L"Çeviri önbelleği temizlendi.",
                              L"Çeviri Önbelleğini Temizle", MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }

        case IDC_INSTALL_LANGPACK_BTN:
        {
            // Bkz. OCREngine.h yorumu: tam otomatik tek-tık kurulum (WinRT
            // LanguagePackManager) kullanıcının SDK sürümünde derlenmediği
            // için, HER SDK/Windows sürümünde çalışan güvenilir yönteme
            // (ms-settings: sayfasını doğrudan açmak) dönüldü.
            ::MessageBoxW(hwnd,
                L"Windows'un \"Dil ve Bölge\" ayar sayfası açılacak. Orada "
                L"ilgili dili seçip \"Seçenekler > Yazı Tanıma'yı ekle\" "
                L"demeniz yeterli.",
                L"OCR Dil Paketi Kur", MB_OK | MB_ICONINFORMATION);
            OCREngine::OpenLanguageSettingsPage();
            return TRUE;
        }

        case IDOK:
            if (SaveToConfig(hwnd, st))
            {
                Logger::Info("SettingsWindow: ayarlar kaydedildi.");
                if (st->onSave) st->onSave(st->config);
                ::EndDialog(hwnd, IDOK);
            }
            return TRUE;

        case IDCANCEL:
            Logger::Info("SettingsWindow: değişiklikler iptal edildi.");
            ::EndDialog(hwnd, IDCANCEL);
            return TRUE;

        default:
        {
            // Kısayol "Değiştir" düğmeleri: sabit sayıda case yerine ID
            // aralığı kontrolü (bkz. IDC_HOTKEY_BTN_BASE/kMaxHotkeyRows) –
            // kısayol sayısı config'e göre DEĞİŞKEN olduğu için switch-case
            // ile (derleme zamanı sabiti gerektirdiğinden) ifade edilemez.
            int id = LOWORD(wp);
            if (id >= IDC_HOTKEY_BTN_BASE && id < IDC_HOTKEY_BTN_BASE + kMaxHotkeyRows)
            {
                int idx = id - IDC_HOTKEY_BTN_BASE;
                if (idx < static_cast<int>(st->config.hotkeys.size()))
                {
                    if (s_recIndex == idx)
                        FinishRecording(false); // "İptal (ESC)" düğmesine tıklandı
                    else
                        StartRecording(hwnd, st, idx);
                    return TRUE;
                }
            }
            break;
        }
        }
        break;
    }

    case WM_VSCROLL:
    {
        // Trackbar'lar (opaklık kaydırıcısı) WM_HSCROLL kullanır; bu, YALNIZCA
        // diyaloğun KENDİ dikey kaydırma çubuğu (bkz. CreateControls'daki
        // WS_VSCROLL) için – lp burada her zaman 0'dır (bir kontrolden değil,
        // doğrudan pencereden gelir).
        auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!st || !st->scrollEnabled) return TRUE;

        RECT client{}; ::GetClientRect(hwnd, &client);
        int pageH = client.bottom - client.top;
        int lineH = 30;

        int newPos = st->scrollPos;
        switch (LOWORD(wp))
        {
        case SB_LINEUP:        newPos -= lineH; break;
        case SB_LINEDOWN:      newPos += lineH; break;
        case SB_PAGEUP:        newPos -= pageH; break;
        case SB_PAGEDOWN:      newPos += pageH; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: newPos = HIWORD(wp); break;
        case SB_TOP:           newPos = 0; break;
        case SB_BOTTOM:        newPos = st->contentHeight; break;
        default: return TRUE;
        }
        UpdateScroll(hwnd, st, newPos);
        return TRUE;
    }

    case WM_MOUSEWHEEL:
    {
        auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!st || !st->scrollEnabled) return TRUE;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step  = (delta > 0 ? -1 : 1) * 60; // bildirim başına ~60px
        UpdateScroll(hwnd, st, st->scrollPos + step);
        return TRUE;
    }

    case WM_DESTROY:
    {
        // Güvenlik ağı: kullanıcı kayıt sırasında (Alt+F4, vs.) diyaloğu
        // beklenmedik şekilde kapatırsa, sistem genelindeki klavye kancasının
        // AÇIK KALMAMASI kritik önemde – aksi hâlde uygulama çökmüş/kapanmış
        // olsa bile TÜM SİSTEMDEKİ klavye girdisi etkilenebilirdi.
        if (s_recDialogHwnd == hwnd)
            FinishRecording(false);
        break;
    }

    case WM_CLOSE:
        ::EndDialog(hwnd, IDCANCEL);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

// ─── Public API ────────────────────────────────────────────────────────────────
void SettingsWindow::Show(HINSTANCE hInstance, HWND owner, const AppConfig& current,
                           std::function<void(const AppConfig&)> onSave,
                           std::function<void()> onClearCache)
{
    static bool s_commCtrlInit = false;
    if (!s_commCtrlInit)
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS };
        ::InitCommonControlsEx(&icc);
        s_commCtrlInit = true;
    }

    State st;
    st.hInstance    = hInstance;
    st.config       = current;
    st.onSave       = std::move(onSave);
    st.onClearCache = std::move(onClearCache);

    std::vector<BYTE> tmpl = BuildEmptyDialogTemplate(L"ScreenLance – Ayarlar");

    ::DialogBoxIndirectParamW(hInstance,
        reinterpret_cast<LPCDLGTEMPLATE>(tmpl.data()),
        owner, DlgProc, reinterpret_cast<LPARAM>(&st));
}
