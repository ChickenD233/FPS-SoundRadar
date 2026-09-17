#include "capture.h"

#include "downmix.h" // kChannels

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "avrt")

namespace sr {

static const wchar_t* kInstallHint =
    L"No matching capture endpoint found.\n"
    L"Either install the SoundRadar VAD driver (see driver/ in this repo) - the capture\n"
    L"device name then contains \"SoundRadar\" and \"Loopback\" - or use the free\n"
    L"Voicemeeter Potato path: route game audio to the Voicemeeter Input as 7.1 and\n"
    L"patch the bus to B1; SoundRadar then captures \"Voicemeeter Out B1\".\n"
    L"A custom substring can be set via capture_device in config.json.\n"
    L"Check: Settings > System > Sound > Recording.";

// Converts one packet to float32 and prefix-maps into 8 analyzer channels
// (FL FR C LFE BL BR SL SR); missing channels stay zero.
static void ConvertToFloat8(const BYTE* src, uint32_t frames, const PcmFormat& fmt,
                            float* dst8) {
    uint32_t n = fmt.channels < 8 ? fmt.channels : 8;
    for (uint32_t f = 0; f < frames; ++f) {
        float* d = dst8 + static_cast<size_t>(f) * 8;
        std::memset(d, 0, 8 * sizeof(float));
        const BYTE* s = src + static_cast<size_t>(f) * fmt.blockAlign;
        for (uint32_t c = 0; c < n; ++c) {
            if (fmt.isFloat) {
                d[c] = reinterpret_cast<const float*>(s)[c];
            } else if (fmt.bits == 16) {
                d[c] = reinterpret_cast<const int16_t*>(s)[c] * (1.0f / 32768.0f);
            } else { // 32-bit PCM
                d[c] = reinterpret_cast<const int32_t*>(s)[c] * (1.0f / 2147483648.0f);
            }
        }
    }
}

CaptureClient::~CaptureClient() {
    if (client_) client_->Stop();
    if (event_) CloseHandle(event_);
}

bool CaptureClient::Init(const std::wstring& captureDevice, std::wstring& err) {
    std::vector<DeviceInfo> candidates = SelectCaptureEndpoints(captureDevice);
    if (candidates.empty()) {
        err = kInstallHint;
        return false;
    }
    std::wstring chosenId = candidates.front().id;
    {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enu;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enu)))) {
            err = L"MMDeviceEnumerator creation failed.";
            return false;
        }
        if (FAILED(enu->GetDevice(chosenId.c_str(), &device_))) {
            err = L"GetDevice failed for the selected capture endpoint.";
            return false;
        }
    }
    deviceName_ = candidates.front().name;
    fwprintf(stderr, L"capture: selected endpoint \"%s\"\n", deviceName_.c_str());

    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) {
        err = L"IAudioClient activation failed on the capture endpoint.";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        err = L"GetMixFormat failed on the capture endpoint.";
        return false;
    }
    fmt_.pcm = InspectFormat(mix);
    // 8ch expected; fewer channels are prefix-mapped into the 8 analyzer
    // channels and zero-filled (a stereo Voicemeeter feed still works).
    if (!fmt_.pcm.valid || fmt_.pcm.channels < 1 || fmt_.pcm.channels > 8) {
        wchar_t msg[256];
        swprintf_s(msg, L"Unsupported capture mix format: %s (need 1-8 ch, 16-bit PCM or float32).",
                   DescribeFormat(mix).c_str());
        err = msg;
        CoTaskMemFree(mix);
        return false;
    }
    fmt_.sampleRate = fmt_.pcm.sampleRate;
    fmt_.channels = fmt_.pcm.channels;
    if (fmt_.channels < 8) {
        fwprintf(stderr,
                 L"capture: note - endpoint has %lu channels; only %lu of 8 analyzer "
                 L"channels active\n",
                 static_cast<unsigned long>(fmt_.channels),
                 static_cast<unsigned long>(fmt_.channels));
    }

    const REFERENCE_TIME requested = 100000; // 10 ms in 100ns units
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             requested, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        wchar_t msg[160];
        swprintf_s(msg, L"Capture Initialize failed (hr=0x%08lx).", static_cast<unsigned long>(hr));
        err = msg;
        return false;
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        err = L"CreateEvent failed.";
        return false;
    }
    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        err = L"SetEventHandle failed on capture client.";
        return false;
    }
    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        err = L"GetService(IAudioCaptureClient) failed.";
        return false;
    }
    UINT32 bufferFrames = 0;
    client_->GetBufferSize(&bufferFrames);
    fmt_.bufferFrames = bufferFrames;

    REFERENCE_TIME defPeriod = 0;
    client_->GetDevicePeriod(&defPeriod, nullptr);
    fmt_.periodMs = defPeriod / 10000.0;

    staging_.resize(static_cast<size_t>(bufferFrames) * 8); // always 8ch internally
    return true;
}

void CaptureClient::Run(const Sink& sink, HANDLE quitEvent) {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        fwprintf(stderr, L"capture: IAudioClient::Start failed (hr=0x%08lx)\n",
                 static_cast<unsigned long>(hr));
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        return;
    }

    HANDLE waits[2] = { quitEvent, event_ };
    bool running = true;
    while (running) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break; // quit
        if (w != WAIT_OBJECT_0 + 1) break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
            if (FAILED(hr)) { running = false; break; }

            if (frames > 0) {
                float* dst = staging_.data();
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(dst, 0, static_cast<size_t>(frames) * 8 * sizeof(float));
                } else {
                    ConvertToFloat8(data, frames, fmt_.pcm, dst); // zero-fills missing ch
                }
                sink(dst, frames, static_cast<LONGLONG>(qpcPos));
            }
            capture_->ReleaseBuffer(frames);
        }
    }

    client_->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace sr
