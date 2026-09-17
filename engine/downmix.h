// downmix.h - 7.1 -> stereo/right-mono downmix. Pure functions, no Windows deps.
#pragma once

#include <cstddef>

namespace sr {

constexpr int kChannels = 8; // FL FR C LFE BL BR SL SR (KSAUDIO_SPEAKER_7POINT1_SURROUND)

enum DownmixMode {
    DownmixRightMono = 0, // everything into the RIGHT channel, left stays silent
    DownmixStereo   = 1,  // ITU-style 7.1 -> 2.0
};

struct DownmixConfig {
    DownmixMode mode = DownmixRightMono;
    // Per-channel weights used in RightMono mode. Order: FL FR C LFE BL BR SL SR.
    float weights[kChannels] = { 0.7f, 1.0f, 1.0f, 0.7f, 0.8f, 0.8f, 0.9f, 0.9f };
};

// Soft clipper (odd symmetric, ~linear below |x| < 0.3).
float SoftClip(float x);

// in8: interleaved float, frames * 8 channels.
// out2: interleaved float, frames * 2 channels. No allocation, no globals.
void Downmix8To2(const float* in8, float* out2, size_t frames, const DownmixConfig& cfg);

} // namespace sr
