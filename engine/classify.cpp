#include "classify.h"

#include "floatcmp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sr {

namespace {
constexpr double kPi = 3.14159265358979323846;
const float kLowBank[] = { 100.f, 150.f, 200.f, 250.f, 300.f };
const float kHighBank[] = { 2000.f, 3000.f, 4000.f };
}

ChannelClassifier::ChannelClassifier(const ClassifyConfig& cfg) : cfg_(cfg) {}

void ChannelClassifier::Reset() { *this = ChannelClassifier(cfg_); }

float ChannelClassifier::GoertzelEnergy(float freq, const float* x, size_t n) const {
    double w = 2.0 * kPi * freq / cfg_.sampleRate;
    double cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double s0 = static_cast<double>(x[i]) + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // normalized magnitude ~ sine amplitude at `freq`
    double power = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return static_cast<float>(2.0 * std::sqrt(power < 0 ? 0 : power) / n);
}

void ChannelClassifier::Process(const float* x, size_t n, float gain, bool gateOpen,
                                float burstThreshold) {
    if (n == 0) return;
    gain_ = (gain > 0.0f) ? gain : 1.0f;
    blockMs_ = static_cast<uint64_t>(n * 1000.0 / cfg_.sampleRate);

    // Closed gate: the block is ambient noise floor. End any open burst and
    // drop held state without classifying.
    if (!gateOpen) {
        if (inBurst_) {
            inBurst_ = false;
            sustained_ = false;
        }
        if (nowMs_ >= holdUntilMs_ && held_ != SoundNone) {
            if (getenv("SR_BURST"))
                fprintf(stderr, "    GATECLOSE clear held cls=%d at now=%llu\n", (int)held_,
                        (unsigned long long)nowMs_);
            held_ = SoundNone;
            confident_ = false;
        }
        nowMs_ += blockMs_;
        return;
    }

    // Block features stay on the raw signal: rms and crest are level-invariant,
    // so a quiet footstep has the same crest as a loud one. The adaptive gain
    // only decides whether this block is a burst.
    double sumSq = 0.0;
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float s = x[i];
        float a = std::fabs(s);
        sumSq += static_cast<double>(s) * s;
        if (FCmpGt(a, peak)) peak = a;
    }
    float rms = static_cast<float>(std::sqrt(sumSq / n));
    float crest = FCmpGt(rms, 1e-6f) ? peak / rms : 0.0f;

    // Burst threshold arrives in raw units (analyzer floor + margin), so compare
    // against the raw block rms. The adaptive gain no longer scales this test:
    // that kept the old absolute threshold and lost the quiet steps again.
    const float thresh = FCmpGt(burstThreshold, 0.0f) ? burstThreshold : cfg_.burstThreshold;
    bool burstNow = FCmpGe(rms, thresh);
    if (burstNow) {
        float low = 0.0f, high = 0.0f;
        for (float f : kLowBank) low += GoertzelEnergy(f, x, n);
        for (float f : kHighBank) high += GoertzelEnergy(f, x, n);
        // per-bin averages: the banks have different sizes, raw sums would
        // bias the low/high comparison toward the larger bank
        low /= sizeof(kLowBank) / sizeof(kLowBank[0]);
        high /= sizeof(kHighBank) / sizeof(kHighBank[0]);
        if (!inBurst_) {
            inBurst_ = true;
            sustained_ = false;
            burstMs_ = 0.0f;
            burstLow_ = burstHigh_ = 0.0f;
            burstCrest_ = 0.0f;
        }
        burstMs_ += static_cast<float>(blockMs_);
        burstLow_ += low;
        burstHigh_ += high;
        if (crest > burstCrest_) burstCrest_ = crest;
        if (FCmpGt(burstMs_, cfg_.maxBurstMs)) sustained_ = true;
    } else if (inBurst_) {
        inBurst_ = false;
        OnBurstEnd();
    }

    // hold/expiry of the displayed classification
    if (nowMs_ >= holdUntilMs_ && held_ != SoundNone) {
        held_ = SoundNone;
        confident_ = false;
    }
    nowMs_ += blockMs_;
}

void ChannelClassifier::OnBurstEnd() {
    if (getenv("SR_BURST"))
        fprintf(stderr, "  burst ms=%.0f low=%.5f high=%.5f crest=%.2f\n",
                burstMs_, burstLow_, burstHigh_, burstCrest_);
    if (sustained_ || FCmpGt(burstMs_, cfg_.maxBurstMs)) return;

    bool lowDominant = FCmpGt(burstLow_, cfg_.lowDominantRatio * burstHigh_);
    bool broadband = FCmpGt(burstHigh_, cfg_.broadbandRatio * burstLow_) &&
                     FCmpGe(burstCrest_, cfg_.minCrest);
    bool impact = FCmpLt(burstMs_, cfg_.impactMaxBurstMs) &&
                  FCmpGt(burstHigh_, cfg_.impactHighRatio * burstLow_) &&
                  FCmpGe(burstCrest_, cfg_.impactMinCrest);

    if (impact) {
        // bullet hit: short, crisp, high-band crack without muzzle thump
        Classify(SoundImpact, FCmpGe(burstCrest_, cfg_.minCrest), nowMs_);
    } else if (lowDominant) {
        // record burst end time in the sliding window ring
        burstTimes_[burstCount_ % 16] = nowMs_;
        ++burstCount_;

        // count spacing-matched pairs inside the history window
        int pairs = 0, inWindow = 0;
        uint64_t lo = (nowMs_ > static_cast<uint64_t>(cfg_.historyMs))
                          ? nowMs_ - static_cast<uint64_t>(cfg_.historyMs)
                          : 0;
        size_t n = burstCount_ < 16 ? burstCount_ : 16;
        uint64_t times[16];
        for (size_t i = 0; i < n; ++i) times[i] = burstTimes_[i];
        // simple ascending sort of the ring contents
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (times[j] < times[i]) { uint64_t t = times[i]; times[i] = times[j]; times[j] = t; }
        for (size_t i = 0; i < n; ++i) {
            if (times[i] < lo) continue;
            ++inWindow;
            if (i > 0 && times[i - 1] >= lo) {
                uint64_t gap = times[i] - times[i - 1];
                if (gap >= static_cast<uint64_t>(cfg_.minSpacingMs) &&
                    gap <= static_cast<uint64_t>(cfg_.maxSpacingMs))
                    ++pairs;
            }
        }
        if (getenv("SR_BURST"))
            fprintf(stderr, "    pairs=%d inWindow=%d n=%d now=%llu\n", pairs, inWindow,
                    (int)n, (unsigned long long)nowMs_);
        if (pairs >= 2) Classify(SoundFootstep, pairs >= 2 && inWindow >= 3, nowMs_);
    } else if (broadband) {
        // one-shot: no low-band periodicity observed recently
        Classify(SoundGunshot, FCmpGe(burstCrest_, cfg_.minCrest * 1.2f), nowMs_);
    }
}

void ChannelClassifier::Classify(SoundClass cls, bool confident, uint64_t nowMs) {
    if (getenv("SR_BURST"))
        fprintf(stderr, "    CLASSIFY cls=%d conf=%d now=%llu\n", (int)cls, confident ? 1 : 0,
                (unsigned long long)nowMs);
    held_ = cls;
    confident_ = confident;
    holdUntilMs_ = nowMs + static_cast<uint64_t>(cfg_.holdMs);
}

// --- 8-channel wrapper -------------------------------------------------------

Classifier8::Classifier8(const ClassifyConfig& cfg) {
    for (int c = 0; c < 8; ++c) ch_[c] = ChannelClassifier(cfg);
}

void Classifier8::Reset() {
    for (auto& c : ch_) c.Reset();
}

void Classifier8::Process(const float* in8, size_t frames, float gain, bool gateOpen,
                          float burstThreshold, const float* thresholds8) {
    const size_t chunk = 4096;
    for (size_t off = 0; off < frames; off += chunk) {
        size_t n = (frames - off < chunk) ? frames - off : chunk;
        for (int c = 0; c < 8; ++c) {
            for (size_t f = 0; f < n; ++f) mono_[c][f] = in8[(off + f) * 8 + c];
            const float thr = thresholds8 ? thresholds8[c] : burstThreshold;
            ch_[c].Process(mono_[c], n, gain, gateOpen, thr);
        }
    }
}

} // namespace sr
