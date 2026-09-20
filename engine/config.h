// config.h - flat JSON config at %APPDATA%/SoundRadar/config.json.
// Hand-rolled minimal parse/write: flat keys, numbers, strings, bools,
// and one float array ("weights"). Not a general JSON parser.
#pragma once

#include <string>

#include "analysis.h"
#include "classify.h"
#include "downmix.h"

namespace sr {

struct OverlayConfig {
    bool enabled = true;      // master on/off (tray toggle, config overlay_enabled)
    float lowThreshold = 0.08f;  // level <= low  -> cyan/teal (far/weak)
    float highThreshold = 0.30f; // low..high -> teal->amber, above -> red (near/loud)
    int offsetX = 0;          // radar center offset from screen center, px (right positive)
    int offsetY = 0;          // radar center offset from screen center, px (down positive)
    int radius = 90;          // radar ring radius, px
    int fxPct = 85;           // effects intensity 0-100 (glow/pulse; 0 = flat minimal)
    float sensitivity = 2.5f; // display gain 0.5-4.0; scales arrow brightness
    int edgeWidthPct = 100;   // edge-band thickness multiplier, percent (50-250)
    int edgeLenPct = 100;     // edge-band length multiplier, percent (50-250)
    float detectThreshold = 0.005f; // class-arrow threshold (display units); gate already rejects noise
    bool frontMerge = true;   // merge the two front FL/FR arrows into one
    bool hideImpact = false;  // hide arrows classified as bullet impacts
    bool duckEnabled = true;  // attenuate front class arrows while own keys are held
    float duckFire = 0.85f;   // attenuation 0-1 while LMB is held
    float duckWalk = 0.65f;   // attenuation 0-1 while WASD is held
    int duckReleaseMs = 300;  // attenuation tail after key release, ms
    float duckConeDeg = 50.f; // front cone half-angle the attenuation applies to, +-deg
    int arrowFadeMs = 500;    // arrow fade-out after sound stops, ms

    // --- experimental view compensation --------------------------------
    // Rotate the drawn sound directions by the view angle the player turned, so
    // an arrow shows where the sound sits relative to the current view. The
    // mouse-to-degrees factor comes from the same cm/360 figure that pointer
    // sensitivity sites use to compare games.
    bool mouseTurn = false;        // master switch (experimental)
    double mouseCm360 = 30.0;      // centimeters of mouse travel for a 360 turn
    double mouseDpi = 800.0;       // mouse DPI as set in the mouse software
    double mouseDegPerCount = 0.0; // hand measure; 0 = compute from cm360+DPI
    double mouseTurnSign = 1.0;    // 1 or -1, inverts the rotation direction
    double mouseCalPct = 100.0;    // correction in percent on the computed value
    std::string mouseGame = "cs2"; // which game the sensitivity figure came from
    double mouseSens = 0.0;        // the in-game sensitivity the user typed
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
    ClassifyConfig classify;      // burst/footstep heuristic tuning
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
