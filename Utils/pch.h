#pragma once

// ─── Definitions ──────────────────────────────────────────────────────────────
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// ─── Windows ──────────────────────────────────────────────────────────────────
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <dcomp.h>

// ─── WinRT / C++/WinRT ────────────────────────────────────────────────────────
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.h>

// ─── llama.cpp (Hy-MT2 GGUF çeviri motoru) ────────────────────────────────────
#include <llama.h>
#include <ggml-backend.h>

// ─── STL ──────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <future>
#include <functional>
#include <queue>
#include <array>
#include <algorithm>
#include <limits>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cwctype>
#include <optional>
#include <span>
#include <format>
#include <ranges>
#include <stdexcept>

// ─── Filesystem ───────────────────────────────────────────────────────────────
#include <filesystem>
namespace fs = std::filesystem;

// ─── JSON (nlohmann) ───────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>
using json_t = nlohmann::json;

// ─── Aliases ──────────────────────────────────────────────────────────────────
using namespace std::chrono_literals;
using namespace std::string_literals;

// ─── Lib pragmas ──────────────────────────────────────────────────────────────
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
