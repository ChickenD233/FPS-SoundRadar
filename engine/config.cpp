#include "config.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace sr {

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
    if (GetNumber(json, "radar_radius", d)) cfg.overlay.radius = static_cast<int>(d);
    GetBool(json, "classify_enabled", cfg.classifyEnabled);
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
    f << "  \"classify_enabled\": " << (cfg.classifyEnabled ? "true" : "false") << ",\n";
    f << "  \"autostart\": " << (cfg.autostart ? "true" : "false") << "\n";
    f << "}\n";
    return static_cast<bool>(f);
}

} // namespace sr
