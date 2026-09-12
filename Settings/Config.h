#pragma once
#include "../Utils/Common.h"

// ─── Capture settings ───────────────────────────────────────────────────────
// Çevrilecek alan nasıl belirlenir: kullanıcı her seferinde fareyle bir
// bölge mi seçer, doğrudan yapılandırılmış TÜM monitör(ler) mi kullanılır,
// yoksa yalnızca o an fare imlecinin üzerinde olduğu (aktif) monitör mü
// kullanılır. Hem tek seferlik hem de sürekli çeviri bu ayara göre davranır
// (bkz. Application::TriggerOneShot / StartContinuous).
// NOT: Yeni değerler her zaman SONA eklenir – mevcut config dosyalarında
// int olarak saklanan değerlerin anlamı değişmesin diye ManualSelection=0
// ve FullScreen=1 sabit kalmalı (bkz. Config.cpp Serialize/Deserialize).
enum class CaptureAreaMode { ManualSelection, FullScreen, ActiveMonitor };

struct CaptureConfig
{
    int captureFps   { 60 };
    int ocrFps       {  5 };
    std::vector<int> monitorIndices; // empty = all monitors
    CaptureAreaMode  areaMode{ CaptureAreaMode::ManualSelection };
};

// ─── OCR settings ───────────────────────────────────────────────────────────
struct OcrConfig
{
    int   minCharCount    { 3    };
    int   minWordCount    { 1    };
};

// ─── Translation settings ───────────────────────────────────────────────────
struct TranslationConfig
{
    std::wstring srcLang{ L"en-US" };
    std::wstring dstLang{ L"tr-TR" };
    bool         useGpu { true };    // llama.cpp GPU katman offload'u (n_gpu_layers) – CUDA derlemesinde
                                      // GPU'ya (NVIDIA) offload eder; CUDA olmadan derlenirse zararsızca
                                      // yok sayılır (CPU'ya düşer).
    int          maxTokens{ 512 };   // üretilecek maksimum çeviri token sayısı
    int          contextSize{ 2048 };// llama context penceresi (prompt + çıktı)
    float        temperature{ 0.7f };
    float        topP{ 0.6f };
    int          topK{ 20 };
    float        repetitionPenalty{ 1.05f };
    std::wstring ggufFileName{ L"Hy-MT2-1.8B-Q8_0.gguf" }; // Models\ klasöründeki GGUF dosya adı
};

// ─── Overlay settings ───────────────────────────────────────────────────────
enum class BlurMode { Blur, AverageColor, SemiTransparent };

struct OverlayConfig
{
    BlurMode    blurMode   { BlurMode::AverageColor };
    std::wstring fontFamily{ L"Segoe UI" };
    float        fontSize  { 14.f };
    bool         fontBold  { false };
    bool         fontOutline{ true };
    COLORREF     textColor { RGB(255,255,255) };
    COLORREF     bgColor   { RGB(0,0,0)       };
    float        bgOpacity { 0.70f };
    // Tek seferlik çevirinin ekranda kaç saniye kalacağı (bkz.
    // Application::TranslationWorkerLoop – "8 saniye" artık sabit değil,
    // buradan okunuyor). Ayarlar penceresinde kullanıcı ya bir ön ayar
    // seçebilir ya da kutuya kendi rakamını yazabilir (bkz. SettingsWindow).
    int          oneShotDisplaySeconds{ 8 }; // 1–60 arası makul
};

// ─── Hotkey definition ──────────────────────────────────────────────────────
struct HotkeyDef
{
    std::string  id;          // "toggle_continuous", "one_shot", etc.
    UINT         modifiers;   // MOD_CONTROL | MOD_SHIFT | …
    UINT         vk;          // Virtual key code

    // Ayarlar penceresinin "kısayollar değişti mi?" kontrolü (bkz.
    // Application::OpenSettingsWindow) için gerekli.
    bool operator==(const HotkeyDef& o) const
    { return id == o.id && modifiers == o.modifiers && vk == o.vk; }
    bool operator!=(const HotkeyDef& o) const { return !(*this == o); }
};

// ─── Root config ────────────────────────────────────────────────────────────
struct AppConfig
{
    CaptureConfig     capture;
    OcrConfig         ocr;
    TranslationConfig translation;
    OverlayConfig     overlay;
    std::vector<HotkeyDef> hotkeys;

    // Defaults for hotkeys
    static AppConfig Defaults();
};

// ─── ConfigManager ──────────────────────────────────────────────────────────
class ConfigManager
{
public:
    explicit ConfigManager(const fs::path& configPath);

    void Load();
    void Save() const;

    AppConfig& Get()             noexcept { return m_cfg; }
    const AppConfig& Get() const noexcept { return m_cfg; }

private:
    fs::path  m_path;
    AppConfig m_cfg;

    static json_t Serialize  (const AppConfig&);
    static AppConfig Deserialize(const json_t&);
};
