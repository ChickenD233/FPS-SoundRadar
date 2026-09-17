// render.h - WASAPI render client to the user's real output device.
// Tries exclusive mode (5 ms, event-driven) first for low latency,
// falls back to shared mode (10 ms) with a printed warning.
#pragma once

#include "wasapi_util.h"

#include <functional>
#include <string>
#include <vector>

namespace sr {

class RenderClient {
public:
    struct Format {
        bool exclusive = false;
        uint32_t sampleRate = 48000;   // device rate (internal engine rate is always 48k)
        uint32_t channels = 2;
        PcmFormat pcm;
        uint32_t bufferFrames = 0;     // device frames
        double periodMs = 0.0;         // actual device period in use
        double bufferMs = 0.0;         // actual buffer duration
    };

    // Engine internal format: 48 kHz stereo float, interleaved.
    static constexpr uint32_t kInternalRate = 48000;

    // Called on the render thread; must fill `frames` stereo float frames.
    using Pull = std::function<void(float* outStereo48k, uint32_t frames)>;

    RenderClient() = default;
    ~RenderClient();

    // nameSub: empty = default multimedia render endpoint, else substring match.
    bool Init(const std::wstring& nameSub, std::wstring& err);

    // Blocking render loop (call on a dedicated thread).
    void Run(const Pull& pull, HANDLE quitEvent);

    const Format& GetFormat() const { return fmt_; }
    const std::wstring& DeviceName() const { return deviceName_; }

    // Frames currently queued between "now" and the DAC, via IAudioClock.
    bool EstimateQueuedFrames(uint64_t& framesAhead);

private:
    bool TryInitialize(const WAVEFORMATEX* wfx, bool exclusive,
                       REFERENCE_TIME bufferDur, REFERENCE_TIME period);

    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    Microsoft::WRL::ComPtr<IAudioClock> clock_;
    HANDLE event_ = nullptr;
    Format fmt_;
    std::wstring deviceName_;

    std::vector<float> inBuf_;   // 48k stereo float from engine
    std::vector<float> resBuf_;  // device-rate stereo float after resampling
    std::vector<float> tmpBuf_;  // multichannel expansion scratch (shared mode)
    double resPhase_ = 0.0;      // resampler read position (fractional input frame)
    float resPrevL_ = 0.0f, resPrevR_ = 0.0f;
    bool resPrimed_ = false;
    uint64_t submittedFrames_ = 0; // cumulative device frames submitted
};

} // namespace sr
