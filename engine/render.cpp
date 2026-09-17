#include "render.h"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propvarutil.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "avrt")

namespace sr {

// Fill a WAVEFORMATEXTENSIBLE for the given PCM layout.
static void MakeFormat(WAVEFORMATEXTENSIBLE* wfx, uint32_t rate, uint32_t channels,
                       uint32_t bits, bool isFloat) {
    std::memset(wfx, 0, sizeof(*wfx));
    wfx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->Format.nChannels = static_cast<WORD>(channels);
    wfx->Format.nSamplesPerSec = rate;
    wfx->Format.wBitsPerSample = static_cast<WORD>(bits);
    wfx->Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    wfx->Format.nAvgBytesPerSec = rate * wfx->Format.nBlockAlign;
    wfx->Format.cbSize = 22;
    wfx->Samples.wValidBitsPerSample = static_cast<WORD>(bits);
    wfx->dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    wfx->SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

RenderClient::~RenderClient() {
    if (client_) client_->Stop();
    if (event_) CloseHandle(event_);
}

bool RenderClient::TryInitialize(const WAVEFORMATEX* wfx, bool exclusive,
                                 REFERENCE_TIME bufferDur, REFERENCE_TIME period) {
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    HRESULT hr;
    if (exclusive) {
        hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wfx, nullptr);
        if (hr != S_OK) return false;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, bufferDur, period, wfx, nullptr);
        if (hr == AUDCLNT_E_INVALID_DEVICE_PERIOD) {
            // Retry once with the device minimum period.
            REFERENCE_TIME minPer = 0, defPer = 0;
            if (SUCCEEDED(client_->GetDevicePeriod(&defPer, &minPer)) && minPer > period) {
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                         minPer * 2, minPer, wfx, nullptr);
            }
        }
        return SUCCEEDED(hr);
    }
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDur, 0, wfx, nullptr);
    return SUCCEEDED(hr);
}

bool RenderClient::Init(const std::wstring& nameSub, std::wstring& err) {
    if (nameSub.empty()) {
        device_ = GetDefaultEndpoint(eRender);
        // Never auto-select a virtual endpoint: if the Windows default output
        // is "Voicemeeter Input" (so games route there) or the SoundRadar VAD
        // speaker, rendering into it would feed back into our capture path.
        if (device_) {
            std::wstring defName;
            {
                Microsoft::WRL::ComPtr<IPropertyStore> props;
                if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
                        v.vt == VT_LPWSTR)
                        defName = v.pwszVal;
                    PropVariantClear(&v);
                }
            }
            if (IsVirtualAudioName(defName)) {
                for (const DeviceInfo& d : EnumerateEndpoints(eRender)) {
                    if (!IsVirtualAudioName(d.name)) {
                        device_ = FindEndpointByName(eRender, d.name);
                        break;
                    }
                }
            }
        }
        if (!device_) {
            err = L"No usable render endpoint found (default is virtual, no physical device).";
            return false;
        }
    } else {
        device_ = FindEndpointByName(eRender, nameSub);
        if (!device_) {
            err = L"No render endpoint matching \"" + nameSub + L"\". Use --list-devices.";
            return false;
        }
    }
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
        err = L"IAudioClient activation failed on the render endpoint.";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) {
        err = L"GetMixFormat failed on the render endpoint.";
        return false;
    }

    // Candidate formats, best first.
    WAVEFORMATEXTENSIBLE f48000f32, f48000p16;
    MakeFormat(&f48000f32, kInternalRate, 2, 32, true);
    MakeFormat(&f48000p16, kInternalRate, 2, 16, false);
    const WAVEFORMATEX* candidates[3] = {
        reinterpret_cast<const WAVEFORMATEX*>(&f48000f32),
        reinterpret_cast<const WAVEFORMATEX*>(&f48000p16),
        mix,
    };

    const WAVEFORMATEX* chosen = nullptr;
    bool exclusive = false;

    // 1) Exclusive, 10 ms buffer / 10 ms period. (5 ms crackles on some USB
    //    devices; 10 ms keeps end-to-end under the 30 ms budget.)
    for (const WAVEFORMATEX* c : candidates) {
        if (TryInitialize(c, true, 100000, 100000)) {
            chosen = c;
            exclusive = true;
            break;
        }
    }
    // 2) Shared, 10 ms buffer. Mix format is tried last.
    if (!chosen) {
        fwprintf(stderr,
                 L"render: exclusive mode unavailable, falling back to shared mode "
                 L"(higher latency)\n");
        for (const WAVEFORMATEX* c : candidates) {
            if (TryInitialize(c, false, 100000, 0)) {
                chosen = c;
                break;
            }
        }
    }
    if (!chosen) {
        err = L"Render Initialize failed for every candidate format.";
        CoTaskMemFree(mix);
        return false;
    }

    fmt_.exclusive = exclusive;
    fmt_.pcm = InspectFormat(chosen);
    if (!fmt_.pcm.valid) {
        err = L"Negotiated render format is unsupported (need 16-bit PCM or float32).";
        CoTaskMemFree(mix);
        return false;
    }
    fmt_.sampleRate = fmt_.pcm.sampleRate;
    fmt_.channels = fmt_.pcm.channels;

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        err = L"CreateEvent failed.";
        CoTaskMemFree(mix);
        return false;
    }
    if (FAILED(client_->SetEventHandle(event_)) ||
        FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
        err = L"Render client setup failed (event handle / IAudioRenderClient).";
        CoTaskMemFree(mix);
        return false;
    }
    client_->GetService(IID_PPV_ARGS(&clock_)); // optional, used by --measure

    UINT32 bufferFrames = 0;
    client_->GetBufferSize(&bufferFrames);
    fmt_.bufferFrames = bufferFrames;
    REFERENCE_TIME defPer = 0, minPer = 0;
    client_->GetDevicePeriod(&defPer, &minPer);
    fmt_.periodMs = (exclusive ? minPer : defPer) / 10000.0;
    fmt_.bufferMs = bufferFrames * 1000.0 / fmt_.sampleRate;

    // Scratch buffers sized for a full buffer worth of frames.
    size_t maxIn = static_cast<size_t>(bufferFrames) * kInternalRate / fmt_.sampleRate + 64;
    inBuf_.resize(maxIn * 2);
    resBuf_.resize((static_cast<size_t>(bufferFrames) + 64) * 2);
    tmpBuf_.resize((static_cast<size_t>(bufferFrames) + 64) * fmt_.channels);

    CoTaskMemFree(mix);
    return true;
}

bool RenderClient::EstimateQueuedFrames(uint64_t& framesAhead) {
    if (!clock_) return false;
    UINT64 pos = 0, qpc = 0;
    if (FAILED(clock_->GetPosition(&pos, &qpc))) return false;
    framesAhead = (submittedFrames_ > pos) ? (submittedFrames_ - pos) : 0;
    return true;
}

void RenderClient::Run(const Pull& pull, HANDLE quitEvent) {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Prime the buffer fully before starting the stream.
    {
        UINT32 pad = 0;
        client_->GetCurrentPadding(&pad);
        UINT32 avail = fmt_.bufferFrames - pad;
        if (avail > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(render_->GetBuffer(avail, &data))) {
                std::memset(data, 0, static_cast<size_t>(avail) * fmt_.pcm.blockAlign);
                render_->ReleaseBuffer(avail, 0);
                submittedFrames_ += avail;
            }
        }
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        fwprintf(stderr, L"render: IAudioClient::Start failed (hr=0x%08lx)\n",
                 static_cast<unsigned long>(hr));
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        return;
    }

    const double step = static_cast<double>(kInternalRate) / fmt_.sampleRate; // >1 if device slower
    const bool needResample = (fmt_.sampleRate != kInternalRate);
    const bool expandChannels = fmt_.channels > 2;

    HANDLE waits[2] = { quitEvent, event_ };
    while (true) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) break;

        UINT32 pad = 0;
        if (FAILED(client_->GetCurrentPadding(&pad))) break;
        UINT32 avail = fmt_.bufferFrames - pad;
        if (avail == 0) continue;

        // Engine produces this many 48k stereo frames for `avail` device frames.
        uint32_t inFrames = needResample
            ? static_cast<uint32_t>(avail * step) + 2
            : avail;
        if (inFrames * 2 > inBuf_.size()) inFrames = static_cast<uint32_t>(inBuf_.size() / 2);
        pull(inBuf_.data(), inFrames);

        float* src = inBuf_.data();
        if (needResample) {
            // Linear resampler, stereo, running phase kept across calls.
            double pos = resPhase_;
            uint32_t out = 0;
            while (out < avail) {
                size_t i0 = static_cast<size_t>(pos);
                float frac = static_cast<float>(pos - i0);
                float l0, r0, l1, r1;
                if (i0 == 0 && !resPrimed_) { l0 = r0 = 0.0f; }
                else if (i0 == 0) { l0 = resPrevL_; r0 = resPrevR_; }
                else { l0 = src[(i0 - 1) * 2]; r0 = src[(i0 - 1) * 2 + 1]; }
                if (i0 >= inFrames) { l1 = l0; r1 = r0; }
                else { l1 = src[i0 * 2]; r1 = src[i0 * 2 + 1]; }
                resBuf_[out * 2] = l0 + frac * (l1 - l0);
                resBuf_[out * 2 + 1] = r0 + frac * (r1 - r0);
                pos += step;
                ++out;
            }
            resPrevL_ = src[(inFrames - 1) * 2];
            resPrevR_ = src[(inFrames - 1) * 2 + 1];
            resPrimed_ = true;
            resPhase_ = pos - inFrames;
            if (resPhase_ < 0.0) resPhase_ = 0.0;
            src = resBuf_.data();
        }

        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(avail, &data))) break;
        if (expandChannels) {
            // Stereo content into FL/FR of a multichannel mix format, rest silent.
            std::memset(tmpBuf_.data(), 0,
                        static_cast<size_t>(avail) * fmt_.channels * sizeof(float));
            for (uint32_t f = 0; f < avail; ++f) {
                tmpBuf_[static_cast<size_t>(f) * fmt_.channels + 0] = src[f * 2];
                tmpBuf_[static_cast<size_t>(f) * fmt_.channels + 1] = src[f * 2 + 1];
            }
            ConvertFromFloat(tmpBuf_.data(), avail, fmt_.pcm, data);
        } else {
            ConvertFromFloat(src, avail, fmt_.pcm, data);
        }
        render_->ReleaseBuffer(avail, 0);
        submittedFrames_ += avail;
    }

    client_->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace sr
