// meters.h - shared seams between audio threads, overlay, and tray.
// The capture thread publishes a snapshot here; the overlay reads it.
// The tray hot-swaps the downmix mode through an atomic (weights only
// change at startup, so they stay in AppConfig).
#pragma once

#include <atomic>
#include <mutex>

#include "analysis.h"
#include "downmix.h" // DownmixMode

namespace sr {

struct SharedMeters {
    std::mutex mu;
    AnalysisFrame frame;
};

// DownmixMode as int; written by the tray thread, read by the render thread.
inline std::atomic<int> g_downmixMode{ DownmixRightMono };

} // namespace sr
