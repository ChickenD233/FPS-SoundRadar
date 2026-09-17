// capture.h - WASAPI event-driven capture from the SoundRadar loopback endpoint.
#pragma once

#include "wasapi_util.h"

#include <functional>
#include <string>
#include <vector>

namespace sr {

class CaptureClient {
public:
    struct Format {
        uint32_t sampleRate = 48000;
        uint32_t channels = 8;
        uint32_t bufferFrames = 0;   // actual capture buffer size
        double periodMs = 0.0;       // device period (default)
        PcmFormat pcm;
    };

    // Called on the capture thread for every packet, with converted float32
    // interleaved frames and the packet's QPC capture timestamp.
    using Sink = std::function<void(const float* frames, uint32_t nFrames, LONGLONG qpcPosition)>;

    CaptureClient() = default;
    ~CaptureClient();

    // Finds the "SoundRadar"+"Loopback" capture endpoint and initializes a
    // shared-mode, event-driven capture client with a ~10 ms buffer.
    bool Init(std::wstring& err);

    // Blocking capture loop (call on a dedicated thread). Returns when
    // quitEvent is signaled or a fatal stream error occurs.
    void Run(const Sink& sink, HANDLE quitEvent);

    const Format& GetFormat() const { return fmt_; }
    const std::wstring& DeviceName() const { return deviceName_; }

private:
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;
    Format fmt_;
    std::wstring deviceName_;
    std::vector<float> staging_;
};

} // namespace sr
