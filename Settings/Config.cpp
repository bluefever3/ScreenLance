#include "../Utils/pch.h"
#include "Config.h"
#include "../Utils/Logger.h"

// ─── Default config factory ─────────────────────────────────────────────────
AppConfig AppConfig::Defaults()
{
    AppConfig cfg;
    cfg.hotkeys = {
        { "toggle_continuous", MOD_CONTROL,  VK_F8   },
        { "one_shot",          MOD_CONTROL,  VK_F9   },
        { "open_settings",     MOD_CONTROL,  VK_F10  },
        { "exit",              MOD_ALT,      VK_F12  },
    };
    return cfg;
}

// ─── JSON helpers ────────────────────────────────────────────────────────────
static std::string WToA(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(sz - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), sz, nullptr, nullptr);
    return s;
}

static std::wstring AToW(const std::string& s)
{
    if (s.empty()) return {};
    int sz = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> buf(sz);
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), sz);
    return std::wstring(buf.begin(), buf.end() - 1);  // exclude null terminator
}

// ─── Serialization ──────────────────────────────────────────────────────────
json_t ConfigManager::Serialize(const AppConfig& c)
{
    json_t j;

    j["capture"]["captureFps"]  = c.capture.captureFps;
    j["capture"]["ocrFps"]      = c.capture.ocrFps;
    j["capture"]["monitors"]    = c.capture.monitorIndices;
    j["capture"]["areaMode"]    = static_cast<int>(c.capture.areaMode);

    j["ocr"]["minCharCount"]    = c.ocr.minCharCount;
    j["ocr"]["minWordCount"]    = c.ocr.minWordCount;

    j["translation"]["srcLang"] = WToA(c.translation.srcLang);
    j["translation"]["dstLang"] = WToA(c.translation.dstLang);
    j["translation"]["useGpu"]  = c.translation.useGpu;
    j["translation"]["maxTokens"]= c.translation.maxTokens;
    j["translation"]["contextSize"]        = c.translation.contextSize;
    j["translation"]["temperature"]        = c.translation.temperature;
    j["translation"]["topP"]               = c.translation.topP;
    j["translation"]["topK"]               = c.translation.topK;
    j["translation"]["repetitionPenalty"]  = c.translation.repetitionPenalty;
    j["translation"]["ggufFileName"]       = WToA(c.translation.ggufFileName);

    j["overlay"]["blurMode"]    = static_cast<int>(c.overlay.blurMode);
    j["overlay"]["fontFamily"]  = WToA(c.overlay.fontFamily);
    j["overlay"]["fontSize"]    = c.overlay.fontSize;
    j["overlay"]["fontBold"]    = c.overlay.fontBold;
    j["overlay"]["fontOutline"] = c.overlay.fontOutline;
    j["overlay"]["textColor"]   = static_cast<uint32_t>(c.overlay.textColor);
    j["overlay"]["bgColor"]     = static_cast<uint32_t>(c.overlay.bgColor);
    j["overlay"]["bgOpacity"]   = c.overlay.bgOpacity;
    j["overlay"]["oneShotDisplaySeconds"] = c.overlay.oneShotDisplaySeconds;

    for (auto& hk : c.hotkeys)
    {
        json_t item;
        item["id"]        = hk.id;
        item["modifiers"] = hk.modifiers;
        item["vk"]        = hk.vk;
        j["hotkeys"].push_back(item);
    }
    return j;
}

AppConfig ConfigManager::Deserialize(const json_t& j)
{
    AppConfig c = AppConfig::Defaults();

    auto get = [&](auto& dst, const json_t& node, const char* key)
    {
        if (node.contains(key)) dst = node.at(key).get<std::decay_t<decltype(dst)>>();
    };

    if (j.contains("capture")) {
        auto& cap = j["capture"];
        get(c.capture.captureFps,     cap, "captureFps");
        get(c.capture.ocrFps,         cap, "ocrFps");
        if (cap.contains("monitors"))
            c.capture.monitorIndices = cap["monitors"].get<std::vector<int>>();
        if (cap.contains("areaMode"))
            c.capture.areaMode = static_cast<CaptureAreaMode>(cap["areaMode"].get<int>());
    }

    if (j.contains("ocr")) {
        auto& o = j["ocr"];
        get(c.ocr.minCharCount,       o, "minCharCount");
        get(c.ocr.minWordCount,       o, "minWordCount");
    }

    if (j.contains("translation")) {
        auto& t = j["translation"];
        if (t.contains("srcLang")) c.translation.srcLang = AToW(t["srcLang"].get<std::string>());
        if (t.contains("dstLang")) c.translation.dstLang = AToW(t["dstLang"].get<std::string>());
        get(c.translation.useGpu,    t, "useGpu");
        get(c.translation.maxTokens, t, "maxTokens");
        get(c.translation.contextSize,       t, "contextSize");
        get(c.translation.temperature,       t, "temperature");
        get(c.translation.topP,              t, "topP");
        get(c.translation.topK,              t, "topK");
        get(c.translation.repetitionPenalty, t, "repetitionPenalty");
        if (t.contains("ggufFileName")) c.translation.ggufFileName = AToW(t["ggufFileName"].get<std::string>());
    }

    if (j.contains("overlay")) {
        auto& ov = j["overlay"];
        if (ov.contains("blurMode"))
            c.overlay.blurMode = static_cast<BlurMode>(ov["blurMode"].get<int>());
        if (ov.contains("fontFamily"))
            c.overlay.fontFamily = AToW(ov["fontFamily"].get<std::string>());
        get(c.overlay.fontSize,    ov, "fontSize");
        get(c.overlay.fontBold,    ov, "fontBold");
        get(c.overlay.fontOutline, ov, "fontOutline");
        if (ov.contains("textColor"))
            c.overlay.textColor = static_cast<COLORREF>(ov["textColor"].get<uint32_t>());
        if (ov.contains("bgColor"))
            c.overlay.bgColor   = static_cast<COLORREF>(ov["bgColor"].get<uint32_t>());
        get(c.overlay.bgOpacity,   ov, "bgOpacity");
        get(c.overlay.oneShotDisplaySeconds, ov, "oneShotDisplaySeconds");
        c.overlay.oneShotDisplaySeconds = std::clamp(c.overlay.oneShotDisplaySeconds, 1, 60);
    }

    if (j.contains("hotkeys") && j["hotkeys"].is_array())
    {
        c.hotkeys.clear();
        for (auto& item : j["hotkeys"])
            c.hotkeys.push_back({ item["id"], item["modifiers"], item["vk"] });
    }
    return c;
}

// ─── ConfigManager impl ─────────────────────────────────────────────────────
ConfigManager::ConfigManager(const fs::path& configPath)
    : m_path(configPath)
    , m_cfg(AppConfig::Defaults())
{}

void ConfigManager::Load()
{
    if (!fs::exists(m_path))
    {
        Logger::Info("Config not found – using defaults.");
        Save();
        return;
    }
    try
    {
        std::ifstream f(m_path);
        json_t j;
        f >> j;
        m_cfg = Deserialize(j);
        Logger::Info("Config loaded.");
    }
    catch (const std::exception& ex)
    {
        Logger::ErrorF("Config load failed: {} – using defaults.", ex.what());
        m_cfg = AppConfig::Defaults();
    }
}

void ConfigManager::Save() const
{
    try
    {
        fs::create_directories(m_path.parent_path());
        std::ofstream f(m_path);
        f << Serialize(m_cfg).dump(4);
        Logger::Info("Config saved.");
    }
    catch (const std::exception& ex)
    {
        Logger::ErrorF("Config save failed: {}", ex.what());
    }
}
