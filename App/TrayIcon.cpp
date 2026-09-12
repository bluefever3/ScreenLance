#include "../Utils/pch.h"
#include "TrayIcon.h"
#include "../Utils/Logger.h"
#include "AppResource.h"

TrayIcon::TrayIcon(HWND messageWindow, HINSTANCE hInstance)
    : m_hwnd(messageWindow), m_hInstance(hInstance)
{}

TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Create(const wchar_t* tooltip)
{
    m_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd             = m_hwnd;
    m_nid.uID              = TRAY_ID;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAY;
    // Uygulamanın kendi ikonu (bkz. App/AppResources.rc) – önceden burada
    // Windows'un jenerik/boş "IDI_APPLICATION" ikonu kullanılıyordu, yani
    // sistem tepsisinde uygulamayı diğerlerinden ayırt etmeye yaramayan
    // varsayılan bir simge görünüyordu. LoadIconW küçük simge boyutlarını
    // (16x16/24x24 gibi) .ico içindeki en uygun karesi seçerek otomatik
    // render eder – bkz. ikonun kendisi zaten çok boyutlu (16-256px)
    // hazırlandı.
    m_nid.hIcon = ::LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!m_nid.hIcon)
        m_nid.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION); // güvenli yedek
    ::wcsncpy_s(m_nid.szTip, tooltip, _TRUNCATE);

    if (!::Shell_NotifyIconW(NIM_ADD, &m_nid))
    {
        Logger::Error("TrayIcon: Shell_NotifyIconW NIM_ADD failed.");
        return false;
    }
    // Set version for better balloon behaviour
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    Logger::Info("TrayIcon created.");
    return true;
}

void TrayIcon::Destroy()
{
    if (m_nid.hWnd)
    {
        ::Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_nid.hWnd = nullptr;
    }
}

void TrayIcon::SetContinuousRunning(bool running)
{
    m_continuousRunning = running;
}

void TrayIcon::OnTrayMessage(LPARAM lParam)
{
    UINT event = LOWORD(lParam);
    if (event == WM_RBUTTONUP || event == NIN_KEYSELECT)
        ShowContextMenu();
}

void TrayIcon::ShowContextMenu()
{
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    if (m_continuousRunning)
        ::AppendMenuW(menu, MF_STRING, IDM_STOP,     L"Sürekli Çeviriyi Durdur");
    else
        ::AppendMenuW(menu, MF_STRING, IDM_START,    L"Sürekli Çeviriyi Başlat");

    ::AppendMenuW(menu, MF_STRING,    IDM_ONESHOT,   L"Tek Seferlik Çeviri");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING,    IDM_SETTINGS,  L"Ayarlar");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING,    IDM_EXIT,      L"Çıkış");

    // Get cursor position
    POINT pt{};
    ::GetCursorPos(&pt);

    // Required trick: set foreground window so menu dismisses properly
    ::SetForegroundWindow(m_hwnd);
    UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                pt.x, pt.y, 0, m_hwnd, nullptr);
    ::DestroyMenu(menu);

    if (!m_callback) return;
    switch (cmd)
    {
    case IDM_START:    m_callback(TrayCommand::StartContinuous); break;
    case IDM_STOP:     m_callback(TrayCommand::StopContinuous);  break;
    case IDM_ONESHOT:  m_callback(TrayCommand::OneShot);         break;
    case IDM_SETTINGS: m_callback(TrayCommand::OpenSettings);    break;
    case IDM_EXIT:     m_callback(TrayCommand::Exit);            break;
    }
}
