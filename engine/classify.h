// classify.h - experimental per-channel sound classification.
// Pure C++, unit-testable. Runs on the capture thread, one instance per
// channel - channels are never merged.
//
// Heuristics (cheap: small Goertzel bank per block + tiny state machine):
//   FOOTSTEP: low-band (100-300 Hz) dominant burst, < 250 ms, repeating
//             with 250-700 ms spacing inside a sliding 3 s window.
//   GUNSHOT:  broadband one-shot transient (high crest factor, energy above
//             2 kHz comparable to the low band, no periodicity).
//   IMPACT:   bullet-hit crack - very short (< 80 ms), high-band dominant,
//             sharp crest, no low-band muzzle thump.
//   else NONE.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sr {

enum SoundClass : uint8_t {
    SoundNone = 0,
    SoundFootstep = 1,
    SoundGunshot = 2,
    SoundImpact = 3,
};

struct ClassifyConfig {
    float sampleRate = 48000.0f;
    float burstThreshold = 0.04f;  // block RMS that starts a burst
    float maxBurstMs = 250.0f;     // longer bursts are "sustained", never classified
    float lowDominantRatio = 2.5f; // lowE > ratio * highE -> low-band dominant
    float broadbandRatio = 0.4f;   // highE > ratio * lowE  -> broadband
    float minCrest = 2.0f;         // peak/RMS for a sharp transient
    float minSpacingMs = 250.0f;   // footstep repetition window
    float maxSpacingMs = 700.0f;
    float impactMaxBurstMs = 60.0f; // impacts are shorter bursts than gunshots
    float impactHighRatio = 2.0f;   // highE > ratio * lowE -> bullet impact
    float impactMinCrest = 1.8f;    // peak/RMS for an impact transient
    float historyMs = 3000.0f;     // sliding window for burst history
    float holdMs = 600.0f;         // how long a classification stays visible
};

class ChannelClassifier {
public:
    explicit ChannelClassifier(const ClassifyConfig& cfg = ClassifyConfig());

    void Reset();
    // One mono block (any length; capture packets are ~10 ms at 48 kHz).
    void Process(const float* samples, size_t n);

    SoundClass Current() const { return held_; }
    // true when the pattern matched strongly (e.g. 3+ footstep bursts).
    bool Confident() const { return confident_; }

private:
    float GoertzelEnergy(float freq, const float* x, size_t n) const;
    void OnBurstEnd();
    void Classify(SoundClass cls, bool confident, uint64_t nowMs);

    ClassifyConfig cfg_;
    uint64_t nowMs_ = 0;
    uint64_t blockMs_ = 0; // duration of the current block, set in Process

    // burst state
    bool inBurst_ = false;
    bool sustained_ = false;
    float burstMs_ = 0.0f;
    float burstLow_ = 0.0f, burstHigh_ = 0.0f, burstCrest_ = 0.0f;

    // footstep repetition history (burst end times, sliding window)
    uint64_t burstTimes_[16] = {};
    size_t burstCount_ = 0; // ring counter

    SoundClass held_ = SoundNone;
    bool confident_ = false;
    uint64_t holdUntilMs_ = 0;
};

// 8 independent channels around one interleaved 7.1 block.
class Classifier8 {
public:
    explicit Classifier8(const ClassifyConfig& cfg = ClassifyConfig());
    void Reset();
    void Process(const float* in8, size_t frames);
    SoundClass ClassOf(int ch) const { return ch_[ch].Current(); }
    bool Confident(int ch) const { return ch_[ch].Confident(); }

private:
    ChannelClassifier ch_[8];
    float mono_[8][4096]; // deinterleave scratch; blocks larger than 4096 are chunked
};

} // namespace sr
