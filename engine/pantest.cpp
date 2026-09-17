// pantest.cpp - plays a channel-panning test pattern on the SoundRadar VAD
// speaker endpoint (shared mode). Acceptance aid: each channel gets a
// distinct-frequency sine so the overlay/loopback path can be eyeballed.
#include "pantest.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "wasapi_util.h"

namespace sr {

namespace {

constexpr double kPi = 3.14159265358979323846;
const wchar_t* kNames[8] = { L"FL", L"FR", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR" };

struct Seg {
    enum Kind { Silence, Single, Dual, All } kind = Silence;
    int ch = -1, ch2 = -1;      // Single: ch; Dual: ch+ch2; All: all channels
    double freq = 0, freq2 = 0; // Hz
    uint64_t start = 0, len = 0; // in frames
};

const wchar_t* ChName(int ch, uint32_t channels) {
    if (channels == 8 && ch >= 0 && ch < 8) return kNames[ch];
    return L"ch";
}

} // namespace

int RunPanTest(int seconds) {
    ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }

    auto dev = FindEndpointByName(eRender, L"SoundRadar", L"Speaker");
    if (!dev) {
        std::printf(
            "SoundRadar speaker endpoint not found.\n"
            "Install the SoundRadar VAD driver first (see driver/ in this repo), then check\n"
            "that a playback device whose name contains \"SoundRadar\" and \"Speaker\"\n"
            "shows up under: Settings > System > Sound > Playback.\n");
        return 2;
    }

    Microsoft::WRL::ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        fwprintf(stderr, L"pan-test: cannot activate SoundRadar speaker endpoint\n");
        return 1;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) {
        fwprintf(stderr, L"pan-test: GetMixFormat failed\n");
        return 1;
    }
    PcmFormat pf = InspectFormat(mix);
    if (!pf.valid) {
        std::fprintf(stderr, "pan-test: unsupported mix format: %s\n",
                     ToUtf8(DescribeFormat(mix)).c_str());
        CoTaskMemFree(mix);
        return 1;
    }
    const uint32_t rate = pf.sampleRate;
    const uint32_t channels = pf.channels; // expect 8; adapt if fewer
    if (rate != 48000)
        std::printf("note: endpoint mix format is %u Hz (expected 48000), adapting\n", rate);
    if (channels != 8)
        std::printf("note: endpoint reports %u channels (expected 8), adapting\n", channels);

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr))) {
        fwprintf(stderr, L"pan-test: Initialize failed\n");
        CoTaskMemFree(mix);
        return 1;
    }
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    client->GetService(IID_PPV_ARGS(&render));
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    std::printf("pan-test: %u Hz, %u ch, buffer %u frames (%.1f ms)\n", rate, channels,
                bufferFrames, bufferFrames * 1000.0 / rate);

    // --- build the pattern ---------------------------------------------------
    std::vector<Seg> segs;
    uint64_t pos = 0;
    auto push = [&](Seg s, double secs) {
        s.start = pos;
        s.len = static_cast<uint64_t>(secs * rate);
        pos += s.len;
        segs.push_back(s);
    };
    const double gap = 0.1;
    for (uint32_t i = 0; i < channels; ++i) {
        Seg s;
        s.kind = Seg::Single;
        s.ch = static_cast<int>(i);
        s.freq = 440.0 + 110.0 * i;
        push(s, 1.0);
        Seg g; // silence
        push(g, gap);
    }
    {
        Seg s;
        s.kind = Seg::Dual;
        s.ch = 0;                                  // FL
        s.ch2 = channels >= 6 ? 5 : channels - 1;  // BR (adapted)
        s.freq = 550.0;
        s.freq2 = 880.0;
        push(s, 2.0);
    }
    {
        Seg s;
        s.kind = Seg::All;
        push(s, 2.0);
    }
    const uint64_t patternFrames = pos;

    // --- announce -------------------------------------------------------------
    for (const Seg& s : segs) {
        if (s.kind == Seg::Single)
            std::printf("  channel %d (%ls): %.0f Hz, 1.0 s\n", s.ch, ChName(s.ch, channels),
                        s.freq);
    }
    std::printf("  dual-direction: FL 550 Hz + BR 880 Hz, 2.0 s\n");
    std::printf("  all %u channels, distinct frequencies, 2.0 s\n", channels);
    if (seconds > 0)
        std::printf("looping pattern (%.1f s) for %d s\n",
                    static_cast<double>(patternFrames) / rate, seconds);

    // --- play ------------------------------------------------------------------
    const double amp = 0.5;
    uint64_t budget = (seconds > 0) ? static_cast<uint64_t>(seconds) * rate : patternFrames;
    std::vector<float> gen(bufferFrames * channels);
    uint64_t produced = 0;
    int curSeg = -2;

    client->Start();
    while (produced < budget) {
        Sleep(5);
        UINT32 pad = 0;
        client->GetCurrentPadding(&pad);
        UINT32 avail = bufferFrames - pad;
        if (avail == 0) continue;

        uint32_t filled = 0;
        for (uint32_t f = 0; f < avail && produced + f < budget; ++f) {
            ++filled;
            uint64_t p = (produced + f) % patternFrames;
            // locate segment
            int si = 0;
            while (si + 1 < static_cast<int>(segs.size()) && p >= segs[si + 1].start) ++si;
            const Seg& s = segs[si];
            if (si != curSeg && s.kind != Seg::Single) {
                if (s.kind == Seg::Dual) std::printf("playing: dual FL+BR\n");
                if (s.kind == Seg::All) std::printf("playing: all channels\n");
            }
            if (si != curSeg && s.kind == Seg::Single)
                std::printf("playing: channel %d (%ls)\n", s.ch, ChName(s.ch, channels));
            curSeg = si;

            double t = static_cast<double>(p - s.start) / rate;
            float* frame = gen.data() + static_cast<size_t>(f) * channels;
            for (uint32_t c = 0; c < channels; ++c) frame[c] = 0.0f;
            switch (s.kind) {
                case Seg::Silence:
                    break;
                case Seg::Single:
                    frame[s.ch] = static_cast<float>(amp * std::sin(2.0 * kPi * s.freq * t));
                    break;
                case Seg::Dual:
                    frame[s.ch] = static_cast<float>(amp * std::sin(2.0 * kPi * s.freq * t));
                    frame[s.ch2] = static_cast<float>(amp * std::sin(2.0 * kPi * s.freq2 * t));
                    break;
                case Seg::All:
                    for (uint32_t c = 0; c < channels; ++c)
                        frame[c] = static_cast<float>(
                            amp * std::sin(2.0 * kPi * (440.0 + 110.0 * c) * t));
                    break;
            }
        }

        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) break;
        if (filled < avail) // zero the tail of the final partial chunk
            std::memset(gen.data() + static_cast<size_t>(filled) * channels, 0,
                        static_cast<size_t>(avail - filled) * channels * sizeof(float));
        ConvertFromFloat(gen.data(), avail, pf, data);
        render->ReleaseBuffer(avail, 0);
        produced += avail;
    }
    // let the tail drain out of the device buffer
    UINT32 pad = bufferFrames;
    while (pad > 0) {
        Sleep(10);
        client->GetCurrentPadding(&pad);
    }
    client->Stop();
    CoTaskMemFree(mix);
    std::printf("pan-test: done\n");
    return 0;
}

} // namespace sr
