#include "config.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace sr {

// config file schema version; bump when retuned defaults must overwrite the
// values old builds persisted (see the migration at the end of LoadConfig).
constexpr int kConfigVersion = 3;

std::wstring DefaultConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? buf : L".";
    return dir + L"\\SoundRadar\\config.json";
}

// --- tiny flat-JSON helpers -------------------------------------------------

// Finds "key" : <value> at top level and returns the raw value text.
static bool FindValue(const std::string& json, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size()) return false;
    size_t end = p;
    if (json[p] == '"') {
        end = json.find('"', p + 1);
        if (end == std::string::npos) return false;
        out = json.substr(p + 1, end - p - 1);
        return true;
    }
    if (json[p] == '[') {
        end = json.find(']', p + 1);
        if (end == std::string::npos) return false;
        out = json.substr(p, end - p + 1);
        return true;
    }
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-' || json[end] == '+' ||
            json[end] == '.' || json[end] == 'e' || json[end] == 'E' ||
            std::isalpha(static_cast<unsigned char>(json[end])))) {
        ++end;
    }
    out = json.substr(p, end - p);
    return !out.empty();
}

static bool GetNumber(const std::string& json, const std::string& key, double& v) {
    std::string raw;
    if (!FindValue(json, key, raw)) return false;
    char* e = nullptr;
    double d = std::strtod(raw.c_str(), &e);
    if (e == raw.c_str()) return false;
    v = d;
    return true;
}

static bool GetBool(const std::string& json, const std::string& key, bool& v) {
    std::string raw;
    if (!FindValue(json, key, raw)) return false;
    if (raw == "true") { v = true; return true; }
    if (raw == "false") { v = false; return true; }
    return false;
}

static bool GetString(const std::string& json, const std::string& key, std::string& v) {
    return FindValue(json, key, v);
}

static bool GetFloatArray(const std::string& json, const std::string& key, float* dst, size_t count) {
    std::string raw;
    if (!FindValue(json, key, raw)) return false;
    if (raw.empty() || raw.front() != '[') return false;
    size_t i = 0;
    const char* p = raw.c_str() + 1;
    while (i < count) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p || *p == ']') break;
        char* e = nullptr;
        float f = std::strtof(p, &e);
        if (e == p) break;
        dst[i++] = f;
        p = e;
    }
    return i == count;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool LoadConfig(const std::wstring& path, AppConfig& cfg) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    std::string s;
    if (GetString(json, "mode", s)) {
        cfg.downmix.mode = (s == "stereo") ? DownmixStereo : DownmixRightMono;
    }
    GetFloatArray(json, "weights", cfg.downmix.weights, kChannels);

    double d;
    if (GetNumber(json, "fade_ms", d)) cfg.analysis.fadeMs = static_cast<int>(d);
    if (GetNumber(json, "activity_threshold", d)) cfg.analysis.activityThreshold = static_cast<float>(d);
    if (GetNumber(json, "peak_threshold", d)) cfg.analysis.peakThreshold = static_cast<float>(d);
    if (GetNumber(json, "attack_ms", d)) cfg.analysis.attackMs = static_cast<float>(d);
    if (GetNumber(json, "release_ms", d)) cfg.analysis.releaseMs = static_cast<float>(d);

    if (GetString(json, "output_device", s)) cfg.outputDevice = Utf8ToWide(s);
    if (GetString(json, "capture_device", s)) cfg.captureDevice = Utf8ToWide(s);
    GetBool(json, "overlay_enabled", cfg.overlay.enabled);
    GetBool(json, "autostart", cfg.autostart);
    if (GetNumber(json, "overlay_low", d)) cfg.overlay.lowThreshold = static_cast<float>(d);
    if (GetNumber(json, "overlay_high", d)) cfg.overlay.highThreshold = static_cast<float>(d);
    if (GetNumber(json, "radar_x", d)) cfg.overlay.offsetX = static_cast<int>(d);
    if (GetNumber(json, "radar_y", d)) cfg.overlay.offsetY = static_cast<int>(d);
    // migration: old defaults (172/180 px below center) -> exact center
    if (cfg.overlay.offsetY == 172 || cfg.overlay.offsetY == 180)
        cfg.overlay.offsetY = 0;
    if (GetNumber(json, "radar_radius", d)) cfg.overlay.radius = static_cast<int>(d);
    if (GetNumber(json, "overlay_fx", d)) cfg.overlay.fxPct = static_cast<int>(d);
    if (GetNumber(json, "sensitivity", d)) cfg.overlay.sensitivity = static_cast<float>(d);
    if (GetNumber(json, "edge_width", d)) cfg.overlay.edgeWidthPct = static_cast<int>(d);
    if (GetNumber(json, "edge_len", d)) cfg.overlay.edgeLenPct = static_cast<int>(d);
    if (GetNumber(json, "detect_threshold", d)) cfg.overlay.detectThreshold = static_cast<float>(d);
    GetBool(json, "front_merge", cfg.overlay.frontMerge);
    GetBool(json, "hide_impact", cfg.overlay.hideImpact);
    GetBool(json, "duck_enabled", cfg.overlay.duckEnabled);
    if (GetNumber(json, "duck_fire", d)) cfg.overlay.duckFire = static_cast<float>(d);
    if (GetNumber(json, "duck_walk", d)) cfg.overlay.duckWalk = static_cast<float>(d);
    if (GetNumber(json, "duck_release_ms", d)) cfg.overlay.duckReleaseMs = static_cast<int>(d);
    if (GetNumber(json, "duck_cone_deg", d)) cfg.overlay.duckConeDeg = static_cast<float>(d);
    if (GetNumber(json, "arrow_fade_ms", d)) cfg.overlay.arrowFadeMs = static_cast<int>(d);
    GetBool(json, "classify_enabled", cfg.classifyEnabled);
    if (GetNumber(json, "classify_burst", d)) cfg.classify.burstThreshold = static_cast<float>(d);
    if (GetNumber(json, "classify_low_ratio", d)) cfg.classify.lowDominantRatio = static_cast<float>(d);
    if (GetNumber(json, "classify_min_crest", d)) cfg.classify.minCrest = static_cast<float>(d);
    if (GetNumber(json, "classify_spacing_min_ms", d)) cfg.classify.minSpacingMs = static_cast<float>(d);
    if (GetNumber(json, "classify_spacing_max_ms", d)) cfg.classify.maxSpacingMs = static_cast<float>(d);
    if (GetNumber(json, "classify_impact_ms", d)) cfg.classify.impactMaxBurstMs = static_cast<float>(d);
    if (GetNumber(json, "classify_impact_ratio", d)) cfg.classify.impactHighRatio = static_cast<float>(d);
    if (GetNumber(json, "classify_impact_crest", d)) cfg.classify.impactMinCrest = static_cast<float>(d);
    GetBool(json, "render_exclusive", cfg.renderExclusive);

    // migration v2: retuned detection thresholds. Old defaults (low 0.15,
    // high 0.5, activity 0.05, detect 0.03, burst 0.04) gated out quiet
    // footsteps and rendered almost everything in the cyan/teal band.
    // Sensitivity and all non-threshold settings are left untouched.
    double ver = 0;
    if (!GetNumber(json, "config_version", ver) || ver < 2) {
        cfg.overlay.lowThreshold = 0.08f;
        cfg.overlay.highThreshold = 0.30f;
        cfg.overlay.detectThreshold = 0.01f;
        cfg.analysis.activityThreshold = 0.02f;
        cfg.classify.burstThreshold = 0.005f;
    }
    // migration v3: adaptive detection. The analyzer now normalizes the levels
    // against the measured noise floor, so the old 0.02 display threshold sits
    // above a quiet step and the burst threshold no longer needs to carry the
    // detection work.
    if (ver < 3) {
        cfg.overlay.detectThreshold = 0.01f;
        cfg.classify.burstThreshold = 0.005f;
    }
    return true;
}

bool SaveConfig(const std::wstring& path, const AppConfig& cfg) {
    // ensure directory exists
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    char num[64];
    f << "{\n";
    f << "  \"config_version\": " << kConfigVersion << ",\n";
    f << "  \"mode\": \"" << (cfg.downmix.mode == DownmixStereo ? "stereo" : "right-mono") << "\",\n";
    f << "  \"weights\": [";
    for (int i = 0; i < kChannels; ++i) {
        std::snprintf(num, sizeof(num), "%.3f", cfg.downmix.weights[i]);
        f << (i ? ", " : "") << num;
    }
    f << "],\n";
    f << "  \"attack_ms\": " << cfg.analysis.attackMs << ",\n";
    f << "  \"release_ms\": " << cfg.analysis.releaseMs << ",\n";
    f << "  \"fade_ms\": " << cfg.analysis.fadeMs << ",\n";
    f << "  \"activity_threshold\": " << cfg.analysis.activityThreshold << ",\n";
    f << "  \"peak_threshold\": " << cfg.analysis.peakThreshold << ",\n";
    f << "  \"output_device\": \"" << WideToUtf8(cfg.outputDevice) << "\",\n";
    f << "  \"capture_device\": \"" << WideToUtf8(cfg.captureDevice) << "\",\n";
    f << "  \"overlay_enabled\": " << (cfg.overlay.enabled ? "true" : "false") << ",\n";
    f << "  \"overlay_low\": " << cfg.overlay.lowThreshold << ",\n";
    f << "  \"overlay_high\": " << cfg.overlay.highThreshold << ",\n";
    f << "  \"radar_x\": " << cfg.overlay.offsetX << ",\n";
    f << "  \"radar_y\": " << cfg.overlay.offsetY << ",\n";
    f << "  \"radar_radius\": " << cfg.overlay.radius << ",\n";
    f << "  \"overlay_fx\": " << cfg.overlay.fxPct << ",\n";
    f << "  \"sensitivity\": " << cfg.overlay.sensitivity << ",\n";
    f << "  \"edge_width\": " << cfg.overlay.edgeWidthPct << ",\n";
    f << "  \"edge_len\": " << cfg.overlay.edgeLenPct << ",\n";
    f << "  \"detect_threshold\": " << cfg.overlay.detectThreshold << ",\n";
    f << "  \"front_merge\": " << (cfg.overlay.frontMerge ? "true" : "false") << ",\n";
    f << "  \"hide_impact\": " << (cfg.overlay.hideImpact ? "true" : "false") << ",\n";
    f << "  \"duck_enabled\": " << (cfg.overlay.duckEnabled ? "true" : "false") << ",\n";
    f << "  \"duck_fire\": " << cfg.overlay.duckFire << ",\n";
    f << "  \"duck_walk\": " << cfg.overlay.duckWalk << ",\n";
    f << "  \"duck_release_ms\": " << cfg.overlay.duckReleaseMs << ",\n";
    f << "  \"duck_cone_deg\": " << cfg.overlay.duckConeDeg << ",\n";
    f << "  \"arrow_fade_ms\": " << cfg.overlay.arrowFadeMs << ",\n";
    f << "  \"classify_enabled\": " << (cfg.classifyEnabled ? "true" : "false") << ",\n";
    f << "  \"classify_burst\": " << cfg.classify.burstThreshold << ",\n";
    f << "  \"classify_low_ratio\": " << cfg.classify.lowDominantRatio << ",\n";
    f << "  \"classify_min_crest\": " << cfg.classify.minCrest << ",\n";
    f << "  \"classify_spacing_min_ms\": " << cfg.classify.minSpacingMs << ",\n";
    f << "  \"classify_spacing_max_ms\": " << cfg.classify.maxSpacingMs << ",\n";
    f << "  \"classify_impact_ms\": " << cfg.classify.impactMaxBurstMs << ",\n";
    f << "  \"classify_impact_ratio\": " << cfg.classify.impactHighRatio << ",\n";
    f << "  \"classify_impact_crest\": " << cfg.classify.impactMinCrest << ",\n";
    f << "  \"render_exclusive\": " << (cfg.renderExclusive ? "true" : "false") << ",\n";
    f << "  \"autostart\": " << (cfg.autostart ? "true" : "false") << "\n";
    f << "}\n";
    return static_cast<bool>(f);
}

} // namespace sr
