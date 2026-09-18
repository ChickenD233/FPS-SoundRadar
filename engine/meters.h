// meters.h - shared seams between audio threads, overlay, tray, and GUI.
// The capture thread publishes a snapshot here; the overlay reads it.
// Config hot-apply: GUI/tray writers bump a version under a short mutex;
// audio/overlay threads copy the struct out once per buffer/frame.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "analysis.h"
#include "classify.h" // SoundClass
#include "config.h"
#include "downmix.h" // DownmixMode

namespace sr {

struct SharedMeters {
    std::mutex mu;
    AnalysisFrame frame;
    uint8_t classes[8] = {};       // SoundClass per channel (classification milestone)
    uint32_t srcChannels = 8;      // actual capture channels (2 = stereo: pan-tracking mode)
};

// Live-downmix snapshot: read once per render buffer (~100 Hz), written by
// the tray/GUI on Apply. Replaces the old atomic mode-only swap.
struct SharedDownmix {
    std::mutex mu;
    DownmixConfig cfg;
};

// Live overlay config; overlay rebuilds its cached geometry when version moves.
struct SharedOverlay {
    std::mutex mu;
    OverlayConfig cfg;
    uint32_t version = 0;
};

// Live analysis config (fade time etc.), applied on the capture thread.
struct SharedAnalysis {
    std::mutex mu;
    AnalysisConfig cfg;
    uint32_t version = 0;
};

inline SharedDownmix g_downmix;
inline SharedOverlay g_overlay;
inline SharedAnalysis g_analysis;

// Classification display toggle (tray/GUI). Read by the overlay thread.
inline std::atomic<bool> g_classifyEnabled{ true };

} // namespace sr
