#include "analysis.h"

#include "floatcmp.h"

#include <cmath>
#include <cstring>

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
    std::memset(dispLevel_, 0, sizeof(dispLevel_));
    std::memset(freeze_, 0, sizeof(freeze_));
    std::memset(silent_, 0, sizeof(silent_));
    std::memset(gate_, 0, sizeof(gate_));
    std::memset(hang_, 0, sizeof(hang_));
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
            // Display smoothing, per sample, for the legacy fixed-threshold
            // display only. The adaptive display reads the block rms instead, so
            // it can react in one block at onset and fall away in a few blocks
            // when the sound stops.
            if (silent_[c] == 0) {
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
    //   The displayed level reads "how far above the channel gate". The gate
    //     threshold is the ambience, so the display is black for ambience and
    //     bright for a step at any captured volume. An earlier version divided
    //     the envelope by the measured floor instead: ambience then read about
    //     0.8 of full scale, which made the color scale meaningless and drew
    //     arrows from noise alone.
    //   The sensitivity slider scales the displayed level, compressed so 4x
    //     does not push every arrow into the red band.
    const float sens = FCmpLt(cfg_.detectSensitivity, 0.1f) ? 0.1f : cfg_.detectSensitivity;
    const float dispSens = std::pow(sens / 2.0f, cfg_.displaySensExp);

    const float gateMargin = cfg_.gateMarginDb;
    const float burstMarginLin = std::pow(10.0f, cfg_.burstMarginDb / 20.0f);
    const float safety = std::pow(10.0f, cfg_.detectMinGainDb / 20.0f);
    const float kneeDb = 4.0f; // soft-open width below the gate threshold
    // Level above the gate that reads as full scale (displaySpanDb, default
    // 20 dB), so the color bands (overlay_low 0.08, overlay_high 0.30) sit
    // 1.2 dB and 4.5 dB above the gate and a loud step still reaches the red
    // band.
    const float dispFullDb = FCmpLt(cfg_.displaySpanDb, 6.0f) ? 6.0f : cfg_.displaySpanDb;
    // Display response, in blocks. Attack one block (10 ms): the first block of
    // a step shows at once. Release a few blocks (~40 ms): a sustained sound
    // reads steady, and the level is down to the gate threshold about 40 ms
    // after the sound stops. The overlay then fades the arrow over its own
    // arrowFadeMs, so a decaying tail must not keep the level up for 250 ms.
    const float dispAttA = 1.0f - std::exp(-msPerBlock / 10.0f);
    const float dispRelA =
        1.0f - std::exp(-msPerBlock / (cfg_.displayReleaseMs > 1.0f ? cfg_.displayReleaseMs : 1.0f));

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
        // No second validation timer. The old gate hold required gateHoldMs of
        // continuous block evidence before the channel counted as live, but that
        // timer and the hang fight each other: once the sound stops the block
        // test goes false, the timer resets to zero, and the channel went silent
        // in the middle of its own fade tail. It also put 20 ms on every onset,
        // because the displayed level was frozen at zero until the timer was
        // satisfied. The gate is already hysteresis (gateHangMs), and both of its
        // inputs are window energies, so a single noise block cannot open it.
        const bool live = gate_[c];

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
        } else {
            // Signal above the channel gate, in units of the gate threshold.
            // The soft-open ramp runs from half the gate threshold (kneeDb
            // below it) to the gate threshold, then dispFullDb above the gate
            // is full scale. A loud step clamps at 1.0; a step just above the
            // ambience still reads about 0.4 and draws.
            const float knee = openAt * std::pow(10.0f, -kneeDb / 20.0f);
            float soft = (rms - knee) / (openAt - knee + 1e-12f);
            if (FCmpLt(soft, 0.0f)) soft = 0.0f;
            if (FCmpGt(soft, 1.0f)) soft = 1.0f;
            const float exc = FCmpGt(rms, openAt) ? (rms / openAt) : 1.0f;
            float target = (exc - 1.0f) * std::pow(10.0f, dispFullDb / gateMargin) * soft;
            target *= dispSens;
            if (FCmpGt(target, 1.0f)) target = 1.0f;
            // Block-level smoothing with a fast attack and a short release.
            // The old display reused disp_, which the per-sample loop smoothed
            // while the channel was not silent: that value was frozen at zero
            // through the gate hold at onset, so the drawn level crawled up over
            // ~100 ms while the sound was already there.
            const float a = FCmpGt(target, dispLevel_[c]) ? dispAttA : dispRelA;
            dispLevel_[c] += a * (target - dispLevel_[c]);
            if (FCmpLt(dispLevel_[c], 1e-4f)) dispLevel_[c] = 0.0f;
            lvl = dispLevel_[c];
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
