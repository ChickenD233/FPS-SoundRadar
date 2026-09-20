// downmix.h - 7.1 -> stereo / right-mono / left-mono downmix. Pure functions,
// no Windows deps.
#pragma once

#include <cstddef>
#include <cstring>

namespace sr {

constexpr int kChannels = 8; // FL FR C LFE BL BR SL SR (KSAUDIO_SPEAKER_7POINT1_SURROUND)

enum DownmixMode {
    DownmixRightMono = 0, // everything into the RIGHT channel, left stays silent
    DownmixStereo   = 1,  // ITU-style 7.1 -> 2.0
    DownmixLeftMono = 2,  // everything into the LEFT channel, right stays silent
};

struct DownmixConfig {
    DownmixMode mode = DownmixStereo; // default: stereo (7.1 spatial image)
    // Per-channel weights used in the mono modes. Order: FL FR C LFE BL BR SL SR.
    float weights[kChannels] = { 0.7f, 1.0f, 1.0f, 0.7f, 0.8f, 0.8f, 0.9f, 0.9f };
};

// Stable names for config, GUI JSON, and --mode. One name for one mode.
inline const char* DownmixModeName(DownmixMode m) {
    switch (m) {
        case DownmixRightMono: return "right-mono";
        case DownmixLeftMono:  return "left-mono";
        default:               return "stereo";
    }
}

// Parses a mode name. Returns false for an unknown name, so a caller can keep
// its current mode instead of silently switching to the default.
inline bool DownmixModeFromName(const char* s, DownmixMode& out) {
    if (!s || !s[0]) return false;
    if (std::strcmp(s, "stereo") == 0) { out = DownmixStereo; return true; }
    if (std::strcmp(s, "right-mono") == 0) { out = DownmixRightMono; return true; }
    if (std::strcmp(s, "left-mono") == 0) { out = DownmixLeftMono; return true; }
    return false;
}

// Soft clipper (odd symmetric, ~linear below |x| < 0.3).
float SoftClip(float x);

// in8: interleaved float, frames * 8 channels.
// out2: interleaved float, frames * 2 channels. No allocation, no globals.
void Downmix8To2(const float* in8, float* out2, size_t frames, const DownmixConfig& cfg);

} // namespace sr
