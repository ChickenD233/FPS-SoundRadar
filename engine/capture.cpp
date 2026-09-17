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
    L"SoundRadar capture endpoint not found.\n"
    L"Install the SoundRadar VAD driver first (see driver/ in this repo),\n"
    L"then check that a capture device whose name contains \"SoundRadar\"\n"
    L"and \"Loopback\" shows up under: Settings > System > Sound > Recording.";

CaptureClient::~CaptureClient() {
    if (client_) client_->Stop();
    if (event_) CloseHandle(event_);
}

bool CaptureClient::Init(std::wstring& err) {
    device_ = FindEndpointByName(eCapture, L"SoundRadar", L"Loopback");
    if (!device_) {
        err = kInstallHint;
        return false;
    }
    // friendly name for logs
    {
        Microsoft::WRL::ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                deviceName_ = v.pwszVal;
            PropVariantClear(&v);
        }
    }

    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) {
        err = L"IAudioClient activation failed on the SoundRadar loopback endpoint.";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        err = L"GetMixFormat failed on the SoundRadar loopback endpoint.";
        return false;
    }
    fmt_.pcm = InspectFormat(mix);
    if (!fmt_.pcm.valid || fmt_.pcm.channels != kChannels) {
        wchar_t msg[256];
        swprintf_s(msg, L"Unsupported loopback mix format: %s (need 8ch, 16-bit PCM or float32).",
                   DescribeFormat(mix).c_str());
        err = msg;
        CoTaskMemFree(mix);
        return false;
    }
    fmt_.sampleRate = fmt_.pcm.sampleRate;
    fmt_.channels = fmt_.pcm.channels;

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

    staging_.resize(static_cast<size_t>(bufferFrames) * fmt_.channels);
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
                    std::memset(dst, 0, static_cast<size_t>(frames) * fmt_.channels * sizeof(float));
                } else {
                    ConvertToFloat(data, frames, fmt_.pcm, dst);
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
