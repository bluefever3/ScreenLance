#include "../Utils/pch.h"
#include "HotkeyManager.h"
#include "../Utils/Logger.h"

HotkeyManager::HotkeyManager(HWND messageWindow)
    : m_hwnd(messageWindow)
{}

HotkeyManager::~HotkeyManager() { UnregisterAll(); }

bool HotkeyManager::RegisterAll(const std::vector<HotkeyDef>& defs)
{
    UnregisterAll();
    bool allOk = true;
    for (auto& def : defs)
    {
        std::wstring reason;
        if (!TryRegister(def, reason))
        {
            Logger::WarningF("Hotkey '{}' registration failed: {}",
                             def.id, std::string(reason.begin(), reason.end()));
            allOk = false;
        }
    }
    return allOk;
}

void HotkeyManager::UnregisterAll()
{
    for (auto& r : m_regs)
        ::UnregisterHotKey(m_hwnd, r.atomId);
    m_regs.clear();
}

bool HotkeyManager::TryRegister(const HotkeyDef& def, std::wstring& outReason)
{
    if (!ValidateCombination(def.modifiers, def.vk, outReason))
        return false;

    int id = m_nextId++;
    if (!::RegisterHotKey(m_hwnd, id, def.modifiers | MOD_NOREPEAT, def.vk))
    {
        outReason = L"This combination is already in use by another application.";
        Logger::WarningF("RegisterHotKey failed for id='{}' (Win32 error {})",
                         def.id, ::GetLastError());
        return false;
    }

    m_regs.push_back({ id, def });
    Logger::InfoF("Hotkey registered: '{}' (id={})", def.id, id);
    return true;
}

bool HotkeyManager::ValidateCombination(UINT mod, UINT vk, std::wstring& outReason)
{
    if (IsReserved(mod, vk))
    {
        outReason = L"Bu kombinasyon Windows tarafından kullanılıyor.";
        return false;
    }
    return true;
}

bool HotkeyManager::IsReserved(UINT mod, UINT vk)
{
    // Win key alone or with L
    if (mod & MOD_WIN) {
        if (vk == 'L' || vk == VK_TAB || vk == VK_DELETE) return true;
        // Win+D, Win+E, Win+R, Win+S etc. – block entire Win modifier
        return true;
    }
    // Ctrl+Alt+Del
    if ((mod & MOD_CONTROL) && (mod & MOD_ALT) && vk == VK_DELETE) return true;
    // Alt+Tab / Alt+F4
    if ((mod & MOD_ALT) && (vk == VK_TAB || vk == VK_F4)) return true;
    // Ctrl+Esc (Start menu)
    if ((mod & MOD_CONTROL) && vk == VK_ESCAPE) return true;
    return false;
}

void HotkeyManager::OnHotkeyMessage(WPARAM wParam)
{
    if (!m_handler) return;
    int id = static_cast<int>(wParam);
    for (auto& r : m_regs)
    {
        if (r.atomId == id)
        {
            Logger::DebugF("Hotkey fired: '{}'", r.def.id);
            m_handler(r.def.id);
            return;
        }
    }
}
