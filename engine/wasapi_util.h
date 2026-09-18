// wasapi_util.h - shared WASAPI helpers: COM init, endpoint enumeration,
// friendly-name matching, format conversion to/from float.
#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sr {

// RAII COM init for the current thread. STA: WebView2 requires it, and WASAPI
// interfaces are free-threaded so the audio threads are unaffected.
struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool Ok() const { return SUCCEEDED(hr); }
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

std::vector<DeviceInfo> EnumerateEndpoints(EDataFlow flow);

// Case-insensitive substring match; all needles must be present.
bool NameContainsAll(const std::wstring& name,
                     const std::wstring& a,
                     const std::wstring& b = L"");

// True for virtual audio endpoints (Voicemeeter buses, SoundRadar VAD).
// Auto render selection must skip these: rendering into Voicemeeter Input
// would feed back into the capture path.
bool IsVirtualAudioName(const std::wstring& name);

// Find first endpoint whose friendly name contains all needles.
Microsoft::WRL::ComPtr<IMMDevice> FindEndpointByName(EDataFlow flow,
                                                     const std::wstring& a,
                                                     const std::wstring& b = L"");

// Returns the default render/capture endpoint for eMultimedia.
Microsoft::WRL::ComPtr<IMMDevice> GetDefaultEndpoint(EDataFlow flow);

// Capture endpoint selection rule (config key capture_device):
// - default "SoundRadar": require "SoundRadar" AND "loopback"; if no match,
//   fall back to "Voicemeeter Out B1", then "Voicemeeter Output" (Potato B1 bus).
// - any other non-empty value: plain case-insensitive substring.
// Returns matching endpoints in priority order (empty = nothing matched).
std::vector<DeviceInfo> SelectCaptureEndpoints(const std::wstring& configValue);

// Inspect a mix/negotiated format. Returns false if unsupported for this engine:
// we accept 16-bit PCM or 32-bit float, any channel count.
struct PcmFormat {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;      // 16 or 32
    bool isFloat = false;   // true = IEEE float32
    uint32_t blockAlign = 0;
    bool valid = false;
};
PcmFormat InspectFormat(const WAVEFORMATEX* wfx);

// Convert a raw capture buffer to interleaved float32. Returns frames.
uint32_t ConvertToFloat(const BYTE* src, uint32_t frames, const PcmFormat& fmt, float* dst);

// Convert interleaved float32 to the target PCM format.
void ConvertFromFloat(const float* src, uint32_t frames, const PcmFormat& fmt, BYTE* dst);

std::wstring DescribeFormat(const WAVEFORMATEX* wfx);

// Wide -> UTF-8, for printing device names on a byte-oriented stream
// (printf %ls uses the C locale and drops non-ASCII characters).
std::string ToUtf8(const std::wstring& w);

} // namespace sr
