// meters.h - shared seams between audio threads, overlay, and tray.
// The capture thread publishes a snapshot here; the overlay reads it.
// The tray hot-swaps the downmix mode through an atomic (weights only
// change at startup, so they stay in AppConfig).
#pragma once

#include <atomic>
#include <mutex>

#include "analysis.h"
#include "classify.h" // SoundClass
#include "downmix.h" // DownmixMode

namespace sr {

struct SharedMeters {
    std::mutex mu;
    AnalysisFrame frame;
    uint8_t classes[8] = {}; // SoundClass per channel (classification milestone)
};

// DownmixMode as int; written by the tray thread, read by the render thread.
inline std::atomic<int> g_downmixMode{ DownmixRightMono };

// Classification display toggle (tray). Read by the overlay thread.
inline std::atomic<bool> g_classifyEnabled{ true };

} // namespace sr
