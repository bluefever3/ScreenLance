#include "../Utils/pch.h"

#include "Logger.h"
#include <ctime>
#include <iomanip>

// UTF-8 string'i wide string'e çevir
static std::wstring UTF8ToWide(std::string_view utf8)
{
    if (utf8.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::vector<wchar_t> buf(len);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf.data(), len);
    return std::wstring(buf.begin(), buf.end());
}

void Logger::Init(const fs::path& logDir, size_t maxBytes)
{
    // Windows 10.0.14393+ (v1607+): UTF-8 console output etkinleştir
    // Bunu yapmazsak OutputDebugStringW'in Unicode çıktısı garbled görünür
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);

    // ── İç scope: mutex burada alınır, kapanan } ile SERBEST bırakılır ──────
    {
        std::lock_guard lock(s_mutex);
        fs::create_directories(logDir);
        s_logDir   = logDir;
        s_maxBytes = maxBytes;

        // 7 günden eski log dosyalarını temizle
        auto now = fs::file_time_type::clock::now();
        for (auto& entry : fs::directory_iterator(logDir))
        {
            if (entry.path().extension() == L".log")
            {
                auto age = now - entry.last_write_time();
                if (age > std::chrono::hours(168))
                    fs::remove(entry.path());
            }
        }

        auto t = std::time(nullptr);
        char ts[32]{};
        std::tm tm{};
        localtime_s(&tm, &t);          // thread-safe (MSVC)
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

        fs::path filePath = logDir / std::format("screenlance_{}.log", ts);
        s_file.open(filePath, std::ios::app | std::ios::out);
        s_written = 0;
        s_ready   = true;
    }
    // ── Mutex burada serbest bırakıldı ──────────────────────────────────────
    // Info() → Write() → tekrar lock almaya çalışır.
    // Scope dışında olduğu için artık deadlock olmaz.
    Info("=== ScreenLance logger started (UTF-8 enabled) ===");
}

void Logger::Shutdown()
{
    std::lock_guard lock(s_mutex);
    if (s_file.is_open())
    {
        s_file.flush();
        s_file.close();
    }
    s_ready = false;
}

void Logger::Write(Level lvl, std::string_view msg)
{
    std::lock_guard lock(s_mutex);
    if (!s_ready) return;

    const char* tag = "INF";
    switch (lvl) {
        case Level::Debug:   tag = "DBG"; break;
        case Level::Warning: tag = "WRN"; break;
        case Level::Error:   tag = "ERR"; break;
        default:             break;
    }

    auto t = std::time(nullptr);
    char ts[32]{};
    std::tm tm{};
    localtime_s(&tm, &t);              // thread-safe (MSVC)
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);   // log satırı için sadece saat

    std::string line = std::format("[{}][{}] {}\n", ts, tag, msg);
    s_file << line;
    s_file.flush();
    s_written += line.size();

    // VS debugger penceresine de yaz — UTF-8 metin için wide string konversiyon
    // gereklidir (Türkçe karakterleri korumak için)
    std::wstring wideLine = UTF8ToWide(line);
    if (!wideLine.empty())
        ::OutputDebugStringW(wideLine.c_str());

    if (s_written >= s_maxBytes)
        Rotate();
}

void Logger::Rotate()
{
    // Write() tarafından lock altında çağrılır – burada tekrar lock alma
    s_file.flush();
    s_file.close();

    std::vector<fs::path> logs;
    for (auto& e : fs::directory_iterator(s_logDir))
        if (e.path().extension() == L".log")
            logs.push_back(e.path());

    std::sort(logs.begin(), logs.end());
    while (logs.size() >= 5)
    {
        fs::remove(logs.front());
        logs.erase(logs.begin());
    }

    auto t = std::time(nullptr);
    char ts[32]{};
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

    fs::path newPath = s_logDir / std::format("screenlance_{}.log", ts);
    s_file.open(newPath, std::ios::out);
    s_written = 0;
    s_file << "[Rotated]\n";
    s_file.flush();
}

void Logger::Debug  (std::string_view msg) { Write(Level::Debug,   msg); }
void Logger::Info   (std::string_view msg) { Write(Level::Info,    msg); }
void Logger::Warning(std::string_view msg) { Write(Level::Warning, msg); }
void Logger::Error  (std::string_view msg) { Write(Level::Error,   msg); }
