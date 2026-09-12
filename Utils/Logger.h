#pragma once
#include "Common.h"

class Logger
{
public:
    enum class Level { Debug, Info, Warning, Error };

    // Call once at startup
    static void Init(const fs::path& logDir, size_t maxBytes = 100 * 1024 * 1024);
    static void Shutdown();

    static void Debug  (std::string_view msg);
    static void Info   (std::string_view msg);
    static void Warning(std::string_view msg);
    static void Error  (std::string_view msg);

    // Formatted variants
    template<typename... Args>
    static void DebugF  (std::format_string<Args...> fmt, Args&&... args)
    { Debug  (std::format(fmt, std::forward<Args>(args)...)); }

    template<typename... Args>
    static void InfoF   (std::format_string<Args...> fmt, Args&&... args)
    { Info   (std::format(fmt, std::forward<Args>(args)...)); }

    template<typename... Args>
    static void WarningF(std::format_string<Args...> fmt, Args&&... args)
    { Warning(std::format(fmt, std::forward<Args>(args)...)); }

    template<typename... Args>
    static void ErrorF  (std::format_string<Args...> fmt, Args&&... args)
    { Error  (std::format(fmt, std::forward<Args>(args)...)); }

private:
    static void Write(Level lvl, std::string_view msg);
    static void Rotate();

    static inline std::mutex      s_mutex;
    static inline std::ofstream   s_file;
    static inline fs::path        s_logDir;
    static inline size_t          s_maxBytes{ 0 };
    static inline size_t          s_written { 0 };
    static inline bool            s_ready   { false };
};
