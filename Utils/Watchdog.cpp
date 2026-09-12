#include "../Utils/pch.h"

#include "Watchdog.h"

Watchdog::Watchdog(std::string_view name,
                   std::chrono::seconds maxSilence,
                   int                  maxCrashes,
                   std::chrono::seconds crashWindow,
                   RestartFn            onRestart)
    : m_name(name)
    , m_maxSilence(maxSilence)
    , m_maxCrashes(maxCrashes)
    , m_crashWindow(crashWindow)
    , m_onRestart(std::move(onRestart))
{
    Ping(); // Initialise last-ping time
}

Watchdog::~Watchdog() { Stop(); }

void Watchdog::Ping()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    m_lastPing.store(now, std::memory_order_relaxed);
}

void Watchdog::Start()
{
    if (m_running.exchange(true)) return;
    Ping();
    m_thread = std::thread(&Watchdog::Loop, this);
}

void Watchdog::Stop()
{
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void Watchdog::Loop()
{
    Logger::InfoF("Watchdog '{}' started (silence={}s, maxCrashes={})",
                  m_name, m_maxSilence.count(), m_maxCrashes);

    while (m_running.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!m_running.load()) break;

        auto now   = std::chrono::steady_clock::now();
        auto last  = std::chrono::steady_clock::time_point(
                         std::chrono::steady_clock::duration(
                             m_lastPing.load(std::memory_order_relaxed)));
        auto diff  = std::chrono::duration_cast<std::chrono::seconds>(now - last);

        if (diff < m_maxSilence) continue;

        Logger::WarningF("Watchdog '{}': no ping for {}s – restarting.", m_name, diff.count());

        // Crash-rate gate
        {
            std::lock_guard lock(m_crashMutex);
            m_crashTimes.push_back(now);

            // Remove old entries outside the window
            m_crashTimes.erase(
                std::remove_if(m_crashTimes.begin(), m_crashTimes.end(),
                    [&](auto& t){ return now - t > m_crashWindow; }),
                m_crashTimes.end());

            if (static_cast<int>(m_crashTimes.size()) >= m_maxCrashes)
            {
                Logger::ErrorF("Watchdog '{}': {} crashes in {}s – giving up.",
                               m_name, m_maxCrashes, m_crashWindow.count());
                m_running.store(false);
                return;
            }
        }

        try
        {
            m_onRestart();
            Ping(); // Reset timer after successful restart
        }
        catch (const std::exception& ex)
        {
            Logger::ErrorF("Watchdog '{}': restart threw: {}", m_name, ex.what());
        }
    }

    Logger::InfoF("Watchdog '{}' stopped.", m_name);
}
