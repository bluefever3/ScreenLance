#pragma once
#include "Common.h"
#include "Logger.h"

// Tracks liveness of a named worker.
// Call Ping() frequently from the worker thread.
// WatchdogThread calls OnDead() if no ping arrives within timeout.
class Watchdog
{
public:
    using RestartFn = std::function<void()>;

    // maxSilence     : how long without a ping before worker is considered dead
    // maxCrashes     : after this many restarts in crashWindow, stop trying
    // crashWindow    : rolling window for crash counting
    // onRestart      : called from watchdog thread to restart the worker
    Watchdog(std::string_view name,
             std::chrono::seconds maxSilence,
             int                  maxCrashes,
             std::chrono::seconds crashWindow,
             RestartFn            onRestart);
    ~Watchdog();

    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void Ping();         // Worker calls this to signal it's alive
    void Start();        // Begin monitoring
    void Stop();         // Stop monitoring (no restart)

    bool IsRunning() const noexcept { return m_running.load(); }

private:
    void Loop();

    std::string              m_name;
    std::chrono::seconds     m_maxSilence;
    int                      m_maxCrashes;
    std::chrono::seconds     m_crashWindow;
    RestartFn                m_onRestart;

    std::atomic<bool>                           m_running{ false };
    std::atomic<std::chrono::steady_clock::time_point::rep> m_lastPing{};

    std::vector<std::chrono::steady_clock::time_point> m_crashTimes;
    std::mutex               m_crashMutex;

    std::thread              m_thread;
};
