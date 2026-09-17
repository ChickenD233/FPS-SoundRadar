// analysis.h - per-channel metering for the overlay. Pure C++, unit-testable.
//
// Channels are NEVER merged: each of the 8 channels keeps its own envelope so
// the overlay can show direction (e.g. rear-left footsteps) independently.
#pragma once

#include <cstddef>

namespace sr {

constexpr int kAnalysisChannels = 8;

struct AnalysisConfig {
    float sampleRate        = 48000.0f;
    float attackMs          = 5.0f;   // fast attack: RMS envelope rise time
    float releaseMs         = 150.0f; // slow release while signal decays
    int   fadeMs            = 400;    // linear fade-to-zero once silence starts (300..500 sensible)
    float smoothMs          = 20.0f;  // display one-pole smoothing (anti-flicker)
    float silenceEps        = 1e-5f;  // block power below this counts as silence
    float peakThreshold     = 0.5f;   // level >= threshold -> peak flag
    float activityThreshold = 0.05f;  // any channel above -> global activity flag
};

struct AnalysisFrame {
    float level[kAnalysisChannels] = {}; // smoothed per-channel level, 0..1
    bool  peak[kAnalysisChannels]  = {}; // level >= peakThreshold
    bool  active = false;                // any channel above activityThreshold
};

class Analyzer {
public:
    explicit Analyzer(const AnalysisConfig& cfg = AnalysisConfig());

    void Reset();
    // Hot-apply a new config (e.g. fade time from the GUI). Resets state.
    void SetConfig(const AnalysisConfig& cfg);
    // in8: interleaved float, frames * 8 channels (pre-downmix, full 7.1).
    void Process(const float* in8, size_t frames, AnalysisFrame& out);
    const AnalysisFrame& Last() const { return last_; }

private:
    AnalysisConfig cfg_;
    float attA_;    // attack one-pole coefficient
    float relA_;    // release one-pole coefficient
    float smA_;     // display smoothing coefficient
    size_t fadeN_;  // fade length in samples

    float env_[kAnalysisChannels];      // power envelope
    float disp_[kAnalysisChannels];     // displayed level
    float freeze_[kAnalysisChannels];   // level frozen at silence onset
    size_t silent_[kAnalysisChannels];  // consecutive silent samples
    AnalysisFrame last_;
};

} // namespace sr
