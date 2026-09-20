#include "analysis.h"

#include "floatcmp.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace sr {

namespace {

float PoleCoef(float ms, float sampleRate) {
    // coefficient for x += a * (target - x) with time constant ms
    return 1.0f - std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

constexpr float kFloorInitDb = -100.0f;

} // namespace

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
    std::memset(gate_, 0, sizeof(gate_));
    std::memset(hang_, 0, sizeof(hang_));
    std::memset(hold_, 0, sizeof(hold_));
    for (int c = 0; c < kAnalysisChannels; ++c) {
        floorDb_[c] = kFloorInitDb;
        detEnv_[c] = 0.0f;
        winIdx_[c] = 0;
        winFill_[c] = 0;
        for (int i = 0; i < kFloorWin; ++i) win_[c][i] = 0.0f;
    }
    gainLin_ = 1.0f;
    last_ = AnalysisFrame{};
}

void Analyzer::Process(const float* in8, size_t frames, AnalysisFrame& out) {
    // Block RMS per channel: one pass, used by the meter, the noise floor, and
    // the gate. The envelope below stays per sample for the fast attack.
    float blockPeak[kAnalysisChannels] = {};
    double blockSumSq[kAnalysisChannels] = {};
    for (size_t f = 0; f < frames; ++f) {
        const float* s = in8 + f * kAnalysisChannels;
        for (int c = 0; c < kAnalysisChannels; ++c) {
            float p = s[c] * s[c];
            // asymmetric one-pole on power: fast attack, slow release
            float a = FCmpGt(p, env_[c]) ? attA_ : relA_;
            env_[c] += a * (p - env_[c]);
            if (FCmpGt(p, blockPeak[c])) blockPeak[c] = p;
            blockSumSq[c] += p;
            if (silent_[c] == 0) {
                // display smoothing (anti-flicker), per sample
                float lvl = std::sqrt(env_[c]);
                disp_[c] += smA_ * (lvl - disp_[c]);
            }
        }
    }

    // --- adaptive detection front end ------------------------------------
    // Per channel, in this order:
    //   1) gate decision against the floor measured in earlier blocks,
    //   2) floor update, but only while the gate is closed. A loud passage
    //      therefore never raises the floor, and a footstep is never absorbed.
    //   3) amplify toward detectFloorDb + detectRangeDb while the gate is open.
    const float msPerBlock = static_cast<float>(frames) * 1000.0f / cfg_.sampleRate;
    // The window is the full ring when the buffer is not yet full, so scanning
    // entries 0..scan-1 is correct in both states. The window length is
    // floorWindowMs rounded to whole blocks.
    const int floorWinBlocks = (cfg_.floorWindowMs > 0)
                                   ? static_cast<int>(cfg_.floorWindowMs * 0.001f *
                                                      cfg_.sampleRate / static_cast<float>(frames) + 0.5f)
                                   : 1;
    const int floorWin = (floorWinBlocks < 1) ? 1
                        : (floorWinBlocks > kFloorWin) ? kFloorWin : floorWinBlocks;
    const float noiseOffsetLin = std::pow(10.0f, cfg_.noiseOffsetDb / 20.0f);
    const float envAttA = 1.0f - std::exp(-msPerBlock / cfg_.gateAttackMs);
    const float envRelA = 1.0f - std::exp(-msPerBlock / cfg_.gateReleaseMs);
    const float floorEps = std::pow(10.0f, cfg_.detectFloorDb / 20.0f); // rms ~ 0.005
    // Block rms per channel, and the gate decision envelope.
    float blkRms[kAnalysisChannels];
    for (int c = 0; c < kAnalysisChannels; ++c) {
        blkRms[c] = static_cast<float>(std::sqrt(blockSumSq[c] / frames));
        float a = FCmpGt(blkRms[c], detEnv_[c]) ? envAttA : envRelA;
        // Parentheses matter here: a * x - x is not a * (x - x).
        detEnv_[c] += a * (blkRms[c] - detEnv_[c]);
    }

    // Gain comes from the floor of the channels that currently carry no sound,
    // taken from the previous block. A loud burst must not change the gain that
    // same block.
    float priorFloor = kFloorInitDb;
    for (int c = 0; c < kAnalysisChannels; ++c) {
        if (gate_[c]) continue;
        if (FCmpGt(floorDb_[c], priorFloor)) priorFloor = floorDb_[c];
    }
    float gainDb;
    if (cfg_.detectAdaptive) {
        gainDb = (cfg_.detectFloorDb + cfg_.detectRangeDb) - priorFloor;
        if (FCmpGt(gainDb, cfg_.detectMaxGainDb)) gainDb = cfg_.detectMaxGainDb;
        if (FCmpLt(gainDb, cfg_.detectMinGainDb)) gainDb = cfg_.detectMinGainDb;
        gainLin_ = std::pow(10.0f, gainDb / 20.0f);
    } else {
        gainLin_ = 1.0f;
    }
    // Display gain and detection gain are separate on purpose.
    //   gainLin_ (from the noise floor) feeds classify and the gate, so a quiet
    //     mix is still detected.
    //   dispGain normalizes the display against the floor plus the detection
    //     headroom (detectRangeDb), so an audible-but-quiet step still draws a
    //     bright arrow, and the color matches the level above the ambience.
    //   The sensitivity slider is NOT part of either one. Scaling the display
    //     by it made 4x push every arrow into the red band, which destroyed the
    //     far/near color grading without improving detection.
    // Display reference is an absolute dBFS level. The analyzer normalizes the
    // raw block level against it, so the color scale keeps its meaning at the
    // reference level and shows a quiet mix brighter. disp_[c] holds raw rms,
    // so the division is exact.
    const float dispRefRms = std::pow(10.0f, cfg_.displayRefDb / 20.0f);

    const float gateMargin = cfg_.gateMarginDb;
    const float burstMarginLin = std::pow(10.0f, cfg_.burstMarginDb / 20.0f);
    const float safety = std::pow(10.0f, cfg_.detectMinGainDb / 20.0f);
    const float kneeDb = 4.0f; // soft-open width below the gate threshold

    out.active = false;
    out.gateOpen = false;
    // Noise floor phase. Per channel, keep the quietest block rms inside a
    // sliding window. The window minimum is a robust ambient estimate: a
    // transient only occupies a few slots, and the minimum drops as soon as the
    // loud slots leave the window, so a level change is followed within
    // floorWindowMs without any tracker tuning.
    float blockBurstRms = 0.0f;     // highest per-channel burst threshold in this block
    float burstThresholds[kAnalysisChannels] = {};
    for (int c = 0; c < kAnalysisChannels; ++c) {
        // Store the smoothed envelope, not the raw block rms: both the floor and
        // the gate then read the same signal, so no noise block can open the
        // gate by spiking above a floor measured from raw blocks.
        float* w = win_[c];
        w[winIdx_[c]] = detEnv_[c];
        winIdx_[c] = (winIdx_[c] + 1) % kFloorWin;
        if (winFill_[c] < kFloorWin) ++winFill_[c];
        const int scan = (winFill_[c] < floorWin) ? winFill_[c] : floorWin;
        float lo = w[0];
        for (int i = 1; i < scan; ++i)
            if (FCmpLt(w[i], lo)) lo = w[i];
        // Deadlock escape (see floorResetMs).
        if (winFill_[c] >= floorWin && hang_[c] == 0 && !gate_[c]) {
            silentBlocks_[c] += frames;
            if (silentBlocks_[c] * 1000 >
                static_cast<size_t>(cfg_.floorResetMs) * static_cast<size_t>(cfg_.sampleRate)) {
                for (int i = 0; i < kFloorWin; ++i) w[i] = detEnv_[c];
                winFill_[c] = kFloorWin;
                lo = detEnv_[c];
                silentBlocks_[c] = 0;
            }
        } else {
            silentBlocks_[c] = 0;
        }
        if (FCmpLt(lo, 1e-7f)) lo = 1e-7f;
        const float floorRms = lo * noiseOffsetLin;
        floorDb_[c] = 20.0f * std::log10(floorRms);
        if (!cfg_.detectAdaptive) {
            floorDb_[c] = kFloorInitDb;
            continue;
        }
        // Gate threshold for this channel: floor + gateMarginDb.
        float openAt = floorRms * std::pow(10.0f, gateMargin / 20.0f);
        if (FCmpLt(openAt, floorEps * safety)) openAt = floorEps * safety;
        openRms_[c] = openAt;
        // Burst threshold for the classifier: floor + burstMarginDb, clamped, so
        // a wrong floor can neither hide all sound nor turn noise into events.
        // The configured classify threshold stays an upper bound, so the GUI
        // slider can still raise it.
        float burstAt = floorRms * burstMarginLin;
        if (FCmpLt(burstAt, cfg_.minBurstRms)) burstAt = cfg_.minBurstRms;
        if (FCmpLt(burstAt, cfg_.configBurstRms)) burstAt = cfg_.configBurstRms;
        if (FCmpGt(burstAt, cfg_.maxBurstRms)) burstAt = cfg_.maxBurstRms;
        burstThresholds[c] = burstAt;
        if (FCmpGt(burstAt, blockBurstRms)) blockBurstRms = burstAt;
    }

    for (int c = 0; c < kAnalysisChannels; ++c) {
        const float rms = blkRms[c];
        const float env = detEnv_[c];
        const float openAt = openRms_[c];
        // The gate opens on the current block and stays open while the smoothed
        // envelope is above the threshold. Opening on the block matters: a step
        // is loudest in its first 10 ms, and waiting for the smoothed envelope
        // loses that onset. Both measures are window energies, so ambient noise
        // cannot open the gate by spiking.
        const bool above = FCmpGe(rms, openAt) || FCmpGe(env, openAt);
        const bool onset = FCmpGe(rms, openAt);
        // The gate needs no warmup: the window minimum is valid from block 1.
        if (!cfg_.detectAdaptive) {
            gate_[c] = FCmpGe(blockPeak[c], cfg_.silenceEps);
        } else if (gate_[c]) {
            if (hang_[c] > 0) hang_[c] -= static_cast<int>(frames);
            gate_[c] = (hang_[c] > 0) || above;
        } else {
            gate_[c] = above;
        }
        if (gate_[c] && above)
            hang_[c] = static_cast<int>(cfg_.gateHangMs * 0.001f * cfg_.sampleRate);
        // The hold counts the block test only: the envelope decays slowly and
        // would otherwise keep the hold satisfied through a long tail.
        if (gate_[c] && onset) hold_[c] += static_cast<int>(frames);
        else hold_[c] = 0;
        // A gate that only just flickers open on noise must not draw. Require
        // gateHoldMs of continuous evidence before the channel reports sound.
        const bool live = gate_[c] &&
                          hold_[c] >= static_cast<int>(cfg_.gateHoldMs * 0.001f * cfg_.sampleRate);

        const bool silent = (!cfg_.detectAdaptive && FCmpLt(blockPeak[c], cfg_.silenceEps)) ||
                            (cfg_.detectAdaptive && !live);
        if (silent) {
            // Silence: freeze the displayed level, then linearly fade it to zero
            // over fadeMs. Deterministic and keeps the meter from lingering.
            if (silent_[c] == 0) freeze_[c] = disp_[c];
            silent_[c] += frames;
            float factor = 1.0f - static_cast<float>(silent_[c]) / static_cast<float>(fadeN_);
            disp_[c] = (factor > 0.0f) ? freeze_[c] * factor : 0.0f;
            if (disp_[c] < 1e-6f) disp_[c] = 0.0f; // reach a true zero, not just small
        } else if (silent_[c] != 0) {
            silent_[c] = 0; // sound returned; smoothing resumes next block
        }

        float lvl;
        if (!cfg_.detectAdaptive) {
            lvl = disp_[c];
        } else if (!live) {
            // Gated out: ambient hiss must not glow on the overlay at all.
            lvl = 0.0f;
        } else {
            // Soft-open ramp: a level that only just clears the gate fades in
            // from zero instead of snapping on.
            const float knee = openAt * std::pow(10.0f, -kneeDb / 20.0f);
            float soft = FCmpGe(env, openAt) ? 1.0f
                                         : (rms - knee) / (openAt - knee + 1e-12f);
            if (FCmpLt(soft, 0.0f)) soft = 0.0f;
            lvl = (disp_[c] / dispRefRms) * soft;
            if (FCmpGt(lvl, 1.0f)) lvl = 1.0f;
        }
        out.level[c] = lvl;
        out.peak[c] = FCmpGe(lvl, cfg_.peakThreshold);
        if (FCmpGe(lvl, cfg_.activityThreshold)) out.active = true;
        // Publish the measured gate, not the display validation hold. The
        // display needs the hold so an isolated noise block cannot draw, but the
        // classifier needs the first block of a step: waiting two blocks loses
        // the onset, which is the loudest part. The classifier has its own
        // threshold, so a noise block cannot start a classification either.
        if (gate_[c]) out.gateOpen = true;
    }
    burstRms_ = blockBurstRms;
    for (int c = 0; c < kAnalysisChannels; ++c)
        if (FCmpLe(burstThresholds[c], 0.0f)) burstThresholds[c] = burstRms_;
    out.detectGain = cfg_.detectAdaptive ? gainLin_ : 1.0f;
    out.detectThreshold = burstRms_;
    {
        float maxFloor = kFloorInitDb;
        for (int c = 0; c < kAnalysisChannels; ++c)
            if (FCmpGt(floorDb_[c], maxFloor)) maxFloor = floorDb_[c];
        out.noiseFloorDbfs = maxFloor;
    }
    for (int c = 0; c < kAnalysisChannels; ++c) burstThreshold_[c] = burstThresholds[c];
    last_ = out;
}

} // namespace sr
