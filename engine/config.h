// config.h - flat JSON config at %APPDATA%/SoundRadar/config.json.
// Hand-rolled minimal parse/write: flat keys, numbers, strings, bools,
// and one float array ("weights"). Not a general JSON parser.
#pragma once

#include <string>

#include "analysis.h"
#include "downmix.h"

namespace sr {

struct OverlayConfig {
    bool enabled = true;      // master on/off (tray toggle, config overlay_enabled)
    float lowThreshold = 0.15f;  // level <= low  -> green
    float highThreshold = 0.5f;  // low..high -> yellow, above -> red
    int offsetX = 0;          // radar center offset from screen center, px (right positive)
    int offsetY = 180;        // radar center offset from screen center, px (down positive)
    int radius = 90;          // radar ring radius, px
    int fxPct = 60;           // effects intensity 0-100 (glow/pulse; 0 = flat minimal)
};

struct AppConfig {
    DownmixConfig downmix;      // mode + weights[8]
    AnalysisConfig analysis;    // thresholds, fade
    std::wstring outputDevice;  // render endpoint name substring, empty = default
    // capture endpoint substring; default "SoundRadar" (also requires
    // "loopback", then falls back to Voicemeeter B1/Output)
    std::wstring captureDevice = L"SoundRadar";
    OverlayConfig overlay;
    bool autostart = false;       // consumed by the autostart milestone
    bool classifyEnabled = true;  // experimental sound classification display
    bool renderExclusive = false; // shared mode by default; exclusive crackles on some USB DACs
};

// Default path: %APPDATA%/SoundRadar/config.json
std::wstring DefaultConfigPath();

// Loads file into cfg; missing file or missing keys keep existing values.
// Returns false only on hard errors (unreadable file).
bool LoadConfig(const std::wstring& path, AppConfig& cfg);

// Writes all keys (pretty, flat). Creates the directory if needed.
bool SaveConfig(const std::wstring& path, const AppConfig& cfg);

} // namespace sr
