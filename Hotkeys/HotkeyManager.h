#pragma once
#include "../Utils/Common.h"
#include "../Settings/Config.h"

using HotkeyHandler = std::function<void(const std::string& id)>;

class HotkeyManager
{
public:
    explicit HotkeyManager(HWND messageWindow);
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // Register all hotkeys from config.  Returns false if any fail.
    bool RegisterAll(const std::vector<HotkeyDef>& defs);

    // Unregister all currently registered hotkeys.
    void UnregisterAll();

    // Try to register one hotkey.
    // Returns true on success.  On failure sets outReason to user-friendly string.
    bool TryRegister(const HotkeyDef& def, std::wstring& outReason);

    // Validates a candidate hotkey combination.
    // Returns true if combination is not a reserved Windows shortcut.
    static bool ValidateCombination(UINT modifiers, UINT vk, std::wstring& outReason);

    // Called from WM_HOTKEY handler.
    void OnHotkeyMessage(WPARAM wParam);

    void SetHandler(HotkeyHandler h) { m_handler = std::move(h); }

private:
    // Windows reserves these combinations
    static bool IsReserved(UINT mod, UINT vk);

    HWND          m_hwnd;
    HotkeyHandler m_handler;

    struct Registration {
        int         atomId;
        HotkeyDef   def;
    };
    std::vector<Registration> m_regs;
    int m_nextId{ 1 };
};
