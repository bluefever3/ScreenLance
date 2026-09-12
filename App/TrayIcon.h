#pragma once
#include "../Utils/Common.h"

enum class TrayCommand {
    StartContinuous,
    StopContinuous,
    OneShot,
    OpenSettings,
    Exit
};

using TrayCallback = std::function<void(TrayCommand)>;

class TrayIcon
{
public:
    TrayIcon(HWND messageWindow, HINSTANCE hInstance);
    ~TrayIcon();

    TrayIcon(const TrayIcon&)            = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Create(const wchar_t* tooltip);
    void Destroy();

    void SetContinuousRunning(bool running);
    void SetCallback(TrayCallback cb) { m_callback = std::move(cb); }

    // Handle WM_APP+1 tray notification message
    void OnTrayMessage(LPARAM lParam);

    static constexpr UINT WM_TRAY = WM_APP + 1;

private:
    void ShowContextMenu();
    void UpdateIcon();

    HWND      m_hwnd;
    HINSTANCE m_hInstance;
    NOTIFYICONDATAW m_nid{};
    bool      m_continuousRunning{ false };
    TrayCallback m_callback;

    static constexpr UINT TRAY_ID      = 1;
    static constexpr UINT IDM_START    = 1001;
    static constexpr UINT IDM_STOP     = 1002;
    static constexpr UINT IDM_ONESHOT  = 1003;
    static constexpr UINT IDM_SETTINGS = 1004;
    static constexpr UINT IDM_EXIT     = 1005;
};
