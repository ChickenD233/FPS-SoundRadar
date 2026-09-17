// overlay.h - transparent always-on-top overlay: center radar + edge bands.
// Rendering: DirectComposition + Direct2D 1.1 (GPU-composited, near-zero CPU).
// Screenshot path uses a WIC render target (no window needed).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "../engine/config.h" // OverlayConfig
#include "../engine/meters.h" // SharedMeters

namespace sr {

class Overlay {
public:
    Overlay() = default;
    ~Overlay(); // calls Stop()

    // Spawns the overlay thread (window + DComp/D2D resources live there).
    // quitEvent: global shutdown; Stop() uses its own event so the tray can
    // toggle the overlay without touching the audio path.
    bool Start(const OverlayConfig& cfg, SharedMeters* meters, HANDLE quitEvent);
    void Stop();

    bool IsRunning() const { return running_.load(); }
    HWND Hwnd() const { return hwnd_.load(); }
    uint64_t FramesDrawn() const { return frames_.load(); }
    // g_overlay.config version the render thread has applied (hot-apply check).
    uint32_t AppliedVersion() const { return appliedVersion_.load(); }

    // Self-measured CPU of the render thread, split into active (~60 fps
    // drawing) and idle (~4 fps polling) buckets since the last ResetStats().
    struct Stats {
        double activeCpuPct = 0.0;
        double idleCpuPct = 0.0;
        uint64_t frames = 0;
    };
    Stats GetStats() const;
    void ResetStats();

private:
    void ThreadMain();

    OverlayConfig cfg_;
    SharedMeters* meters_ = nullptr;
    HANDLE quitEvent_ = nullptr;   // not owned
    HANDLE stopEvent_ = nullptr;   // owned
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint32_t> appliedVersion_{0};
    std::atomic<uint64_t> activeCpu_{0}, activeWall_{0};
    std::atomic<uint64_t> idleCpu_{0}, idleWall_{0};
};

// Renders one overlay frame (levels + optional SoundClass per channel) into a
// BMP via a D2D WIC render target. Independent of the window/swapchain path.
bool RenderSceneToFile(const std::wstring& path, int width, int height,
                       const float levels[8], const uint8_t* classes,
                       const OverlayConfig& cfg);

} // namespace sr
