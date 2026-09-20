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
    float releaseMs         = 90.0f;  // release while signal decays (lower = less lag)
    int   fadeMs            = 250;    // linear fade-to-zero once silence starts
    float smoothMs          = 12.0f;  // display one-pole smoothing (anti-flicker)
    float silenceEps        = 1e-5f;  // block power below this counts as silence
    float peakThreshold     = 0.5f;   // level >= threshold -> peak flag
    float activityThreshold = 0.02f;  // any channel above -> global activity flag

    // --- adaptive detection front end -------------------------------------
    // A game mix at 40% global volume leaves footsteps far below any fixed
    // threshold, so the thresholds must follow the ambient noise level. The
    // analyzer measures that level per channel over a sliding window, amplifies
    // the quiet mix toward a fixed reference, gates true silence, and reports
    // both the gain and a per-channel burst threshold for the classifier.
    bool  detectAdaptive = true;   // false = legacy fixed-threshold behavior
    float detectFloorDb  = -46.0f; // target level for the measured noise floor
    float detectRangeDb  = 22.0f;  // detection headroom kept above the floor
    // Display span, in dB above the measured ambient level. The displayed level
    // reaches 1.0 at floor + displaySpanDb, which maps the color thresholds
    // (overlay_low/high, default 0.08 / 0.30) to 2.4 dB / 9 dB above the
    // ambience. The span follows the measured floor, so a quiet mix stays as
    // visible as a loud one. An absolute dBFS reference was tried and reverted:
    // at 40% game volume the arrows read almost black.
    float displaySpanDb  = 30.0f;
    // Display gain from the sensitivity slider, compressed so the color scale
    // survives a 4x setting: sensGain = (sensitivity / 2)^displaySensExp.
    float displaySensExp = 0.6f;
    float detectMaxGainDb = 36.0f; // gain cap (63x), avoids amplifying dither
    float detectMinGainDb = -6.0f; // gain floor (0.5x)
    float detectSensitivity = 2.0f; // user "sensitivity" 0.5..4.0, 2.0 = neutral
    // Noise floor: the quietest block rms inside a sliding window. The window
    // minimum follows a level change within floorWindowMs, so a loud passage
    // cannot leave the floor stuck high. noiseOffsetDb covers the gap between
    // the quietest block and the mean ambient level.
    int   floorWindowMs  = 1000;
    float noiseOffsetDb  = 6.0f;
    // Envelope must exceed floor + gateMarginDb to open the gate. Lower is more
    // sensitive. Measured on the host harness at a -45 dBFS ambient floor: 8 dB
    // detects steps down to -30 dBFS, 4 dB down to -39, 2 dB down to -42, and
    // 4 dB kept the false-positive count at zero. 4 is the default; the GUI
    // exposes 2..12 as a trade-off control.
    float gateMarginDb   = 4.0f;   // envelope must exceed floor + margin to open
    // 20 ms: two blocks. Long enough that an isolated noise block cannot draw,
    // short enough that the classifier still sees the onset of a short step.
    int   gateHoldMs     = 20;     // gate must stay open this long before it counts
    int   gateHangMs     = 100;    // keep the gate open this long after a burst
    // Deadlock escape: if the whole window is loud (audio starts inside loud
    // content), no block can clear the threshold and the gate would stay shut
    // forever. After this much continuous silence, reset the window to the
    // current level so the detector finds the new baseline.
    int   floorResetMs   = 2000;
    // Gate decision envelope: a 10 ms block of a decaying step falls below the
    // gate mid-burst, which would truncate the burst. The gate reads a smoothed
    // envelope instead: 8 ms attack, 120 ms release.
    float gateAttackMs   = 8.0f;
    float gateReleaseMs  = 120.0f;
    // The classifier burst threshold sits BELOW the gate threshold on purpose. A
    // step is loudest at its onset, and the gate opens a block later (it waits
    // for the block and the envelope). A threshold between the floor and the
    // gate lets that onset block register.
    float burstMarginDb  = 3.0f;   // classifier burst threshold above the floor
    float minBurstRms    = 0.0005f; // lower bound for the classifier burst threshold
    float maxBurstRms    = 0.05f;   // upper bound: a wrong floor must not deafen the detector
    float configBurstRms = 0.0f;    // classify_burst from the config; 0 = ignore
};

struct AnalysisFrame {
    float level[kAnalysisChannels] = {}; // smoothed per-channel level, 0..1
    bool  peak[kAnalysisChannels]  = {}; // level >= peakThreshold
    bool  active = false;                // any channel above activityThreshold
    float detectGain = 1.0f;             // adaptive gain applied to this block
    float detectThreshold = 0.0f;        // burst threshold for the classifier, raw units
    float noiseFloorDbfs = -100.0f;      // measured floor, loudest channel
    bool  gateOpen = false;              // sound present, not just noise
};

class Analyzer {
public:
    explicit Analyzer(const AnalysisConfig& cfg = AnalysisConfig());

    void Reset();
    // Hot-apply a new config (e.g. fade time from the GUI). Resets state.
    void SetConfig(const AnalysisConfig& cfg);
    // in8: interleaved float, frames * 8 channels (pre-downmix, full 7.1).
    void Process(const float* in8, size_t frames, AnalysisFrame& out);    // Multiplier the caller must apply to the same block before classification.
    float DetectGain() const { return gainLin_; }
    // Block RMS that counts as sound rather than ambient noise. The classifier
    // uses this as its burst threshold so the two stages agree on "sound".
    float DetectThreshold() const { return burstRms_; }
    // Per-channel burst thresholds. The classifier needs one threshold for each
    // channel: a single global value either buries quiet steps (when a loud
    // channel sets it) or starts bursts on noise (when a quiet channel sets it).
    const float* BurstThresholds() const { return burstThreshold_; }
    // True when any channel carries sound rather than ambient noise floor.
    bool GateOpen() const { return last_.gateOpen; }
    // Diagnostics: per-channel noise floor (dBFS) and gate-decision envelope.
    float DebugFloorDb(int c) const { return floorDb_[c]; }
    float DebugEnv(int c) const { return detEnv_[c]; }
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
    // adaptive detection state
    static constexpr int kFloorWin = 128; // sliding window, in blocks
    float win_[kAnalysisChannels][kFloorWin] = {}; // block rms history
    int   winIdx_[kAnalysisChannels] = {};         // next slot to write
    int   winFill_[kAnalysisChannels] = {};        // valid slots
    float floorDb_[kAnalysisChannels];  // per-channel noise floor, dBFS
    float openRms_[kAnalysisChannels];  // per-channel gate threshold, rms
    float detEnv_[kAnalysisChannels];   // gate decision envelope, rms
    bool  gate_[kAnalysisChannels];     // per-channel gate latch
    int   hang_[kAnalysisChannels];     // gate hang left, in samples
    int   hold_[kAnalysisChannels];     // continuous open time, in samples
    size_t silentBlocks_[kAnalysisChannels] = {}; // continuous closed-gate time, samples
    float gainLin_ = 1.0f;              // published gain for the classifier
    float burstRms_ = 0.0f;             // highest per-channel burst threshold
    float burstThreshold_[kAnalysisChannels] = {}; // per-channel burst thresholds
    AnalysisFrame last_;};

} // namespace sr
