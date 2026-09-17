#include "analysis.h"

#include <cmath>
#include <cstring>

namespace sr {

static float PoleCoef(float ms, float sampleRate) {
    // coefficient for x += a * (target - x) with time constant ms
    return 1.0f - std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

Analyzer::Analyzer(const AnalysisConfig& cfg) : cfg_(cfg) {
    SetConfig(cfg);
}

void Analyzer::SetConfig(const AnalysisConfig& cfg) {
    cfg_ = cfg;
    attA_  = PoleCoef(cfg_.attackMs, cfg_.sampleRate);
    relA_  = PoleCoef(cfg_.releaseMs, cfg_.sampleRate);
    smA_   = PoleCoef(cfg_.smoothMs, cfg_.sampleRate);
    fadeN_ = static_cast<size_t>(cfg_.fadeMs * 0.001f * cfg_.sampleRate);
    if (fadeN_ == 0) fadeN_ = 1;
    Reset();
}

void Analyzer::Reset() {
    std::memset(env_, 0, sizeof(env_));
    std::memset(disp_, 0, sizeof(disp_));
    std::memset(freeze_, 0, sizeof(freeze_));
    std::memset(silent_, 0, sizeof(silent_));
    last_ = AnalysisFrame{};
}

void Analyzer::Process(const float* in8, size_t frames, AnalysisFrame& out) {
    // Per-sample power envelope + display smoothing; per-block silence detection.
    float blockPeak[kAnalysisChannels] = {};
    for (size_t f = 0; f < frames; ++f) {
        const float* s = in8 + f * kAnalysisChannels;
        for (int c = 0; c < kAnalysisChannels; ++c) {
            float p = s[c] * s[c];
            // asymmetric one-pole on power: fast attack, slow release
            float a = (p > env_[c]) ? attA_ : relA_;
            env_[c] += a * (p - env_[c]);
            if (p > blockPeak[c]) blockPeak[c] = p;
            if (silent_[c] == 0) {
                // display smoothing (anti-flicker), per sample
                float lvl = std::sqrt(env_[c]);
                disp_[c] += smA_ * (lvl - disp_[c]);
            }
        }
    }

    out.active = false;
    for (int c = 0; c < kAnalysisChannels; ++c) {
        if (blockPeak[c] < cfg_.silenceEps) {
            // Silence: freeze the displayed level, then linearly fade it to zero
            // over fadeMs. Deterministic and keeps the meter from lingering.
            if (silent_[c] == 0) freeze_[c] = disp_[c];
            silent_[c] += frames;
            float factor = 1.0f - static_cast<float>(silent_[c]) / static_cast<float>(fadeN_);
            disp_[c] = (factor > 0.0f) ? freeze_[c] * factor : 0.0f;
        } else if (silent_[c] != 0) {
            silent_[c] = 0; // sound returned; smoothing resumes next block
        }
        out.level[c] = disp_[c];
        out.peak[c] = disp_[c] >= cfg_.peakThreshold;
        if (disp_[c] >= cfg_.activityThreshold) out.active = true;
    }
    last_ = out;
}

} // namespace sr
