#include "downmix.h"

#include <cmath>

namespace sr {

float SoftClip(float x) {
    return std::tanh(x);
}

void Downmix8To2(const float* in8, float* out2, size_t frames, const DownmixConfig& cfg) {
    if (cfg.mode != DownmixStereo) {
        // Mono: all 8 channels summed with their weights, then normalized by the
        // average weight so the loudness matches the stereo path. Both output
        // channels carry the same signal.
        const float* w = cfg.weights;
        float wsum = 0.0f;
        for (int c = 0; c < kChannels; ++c) wsum += w[c];
        const float norm = (wsum > 1e-6f) ? (static_cast<float>(kChannels) / wsum) : 1.0f;
        for (size_t f = 0; f < frames; ++f) {
            const float* s = in8 + f * kChannels;
            float sum = w[0] * s[0] + w[1] * s[1] + w[2] * s[2] + w[3] * s[3]
                      + w[4] * s[4] + w[5] * s[5] + w[6] * s[6] + w[7] * s[7];
            const float m = SoftClip(sum * norm);
            out2[f * 2 + 0] = m;
            out2[f * 2 + 1] = m;
        }
    } else {
        // ITU-style: center and surrounds at -3 dB into the nearer side, LFE dropped.
        constexpr float kAtt = 0.7071f;
        for (size_t f = 0; f < frames; ++f) {
            const float* s = in8 + f * kChannels;
            float l = s[0] + kAtt * s[2] + kAtt * s[4] + kAtt * s[6]; // FL C BL SL
            float r = s[1] + kAtt * s[2] + kAtt * s[5] + kAtt * s[7]; // FR C BR SR
            out2[f * 2 + 0] = SoftClip(l);
            out2[f * 2 + 1] = SoftClip(r);
        }
    }
}

} // namespace sr
