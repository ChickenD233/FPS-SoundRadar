// measure.cpp - latency instrumentation.
#include "measure.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

#include "capture.h"
#include "downmix.h"
#include "render.h"
#include "ring.h"

namespace sr {

namespace {

double QpcToSec(LONGLONG qpc, LONGLONG freq) {
    return static_cast<double>(qpc) / static_cast<double>(freq);
}

} // namespace

int RunMeasure(const std::wstring& outputName) {
    ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }

    CaptureClient cap;
    std::wstring err;
    if (!cap.Init(err)) {
        std::fprintf(stderr, "%s\n", ToUtf8(err).c_str());
        return 2;
    }
    RenderClient ren;
    if (!ren.Init(outputName, err)) {
        std::fprintf(stderr, "render init failed: %s\n", ToUtf8(err).c_str());
        return 1;
    }

    RingBuffer ring(8192); // ~170 ms at 48 kHz
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::atomic<uint64_t> ringDepthSum{0}, ringDepthN{0}, ringDepthMax{0};
    std::atomic<uint64_t> capPackets{0}, capFrames{0};
    DownmixConfig dcfg;

    std::thread capThread([&] {
        cap.Run([&](const float* frames, uint32_t n, LONGLONG) {
            ring.Write(frames, n);
            capPackets.fetch_add(1);
            capFrames.fetch_add(n);
        }, quit);
    });

    std::atomic<uint64_t> queuedSum{0}, queuedN{0}, queuedMax{0};
    std::thread renThread([&] {
        std::vector<float> in8, stereo;
        ren.Run([&](float* out, uint32_t frames) {
            if (in8.size() < static_cast<size_t>(frames) * 8) {
                in8.resize(static_cast<size_t>(frames) * 8);
                stereo.resize(static_cast<size_t>(frames) * 2);
            }
            size_t got = ring.Read(in8.data(), frames);
            if (got < frames)
                std::memset(in8.data() + got * 8, 0, (frames - got) * 8 * sizeof(float));
            size_t depth = ring.Available();
            ringDepthSum.fetch_add(depth);
            ringDepthN.fetch_add(1);
            uint64_t prev = ringDepthMax.load();
            while (depth > prev && !ringDepthMax.compare_exchange_weak(prev, depth)) {}
            Downmix8To2(in8.data(), stereo.data(), frames, dcfg);
            std::memcpy(out, stereo.data(), frames * 2 * sizeof(float));
            uint64_t q = 0;
            if (ren.EstimateQueuedFrames(q)) {
                queuedSum.fetch_add(q);
                queuedN.fetch_add(1);
                uint64_t qp = queuedMax.load();
                while (q > qp && !queuedMax.compare_exchange_weak(qp, q)) {}
            }
        }, quit);
    });

    std::printf("measuring for 3 seconds...\n");
    Sleep(3000);
    SetEvent(quit);
    capThread.join();
    renThread.join();
    CloseHandle(quit);

    const CaptureClient::Format& cf = cap.GetFormat();
    const RenderClient::Format& rf = ren.GetFormat();
    double ringAvgMs = ringDepthN ? (double)ringDepthSum / ringDepthN * 1000.0 / cf.sampleRate : 0.0;
    double ringMaxMs = ringDepthMax * 1000.0 / cf.sampleRate;
    double capBufMs = cf.bufferFrames * 1000.0 / cf.sampleRate;
    double queuedAvgMs = queuedN ? (double)queuedSum / queuedN * 1000.0 / rf.sampleRate : 0.0;
    double queuedMaxMs = queuedMax * 1000.0 / rf.sampleRate;

    std::printf("\n--- SoundRadar latency report ---\n");
    std::printf("capture endpoint : %s\n", ToUtf8(cap.DeviceName()).c_str());
    std::printf("  format         : %u Hz, %u ch\n", cf.sampleRate, cf.channels);
    std::printf("  device period  : %.2f ms\n", cf.periodMs);
    std::printf("  buffer depth   : %.2f ms (%u frames)\n", capBufMs, cf.bufferFrames);
    std::printf("  packets        : %llu (%llu frames)\n",
                (unsigned long long)capPackets.load(), (unsigned long long)capFrames.load());
    std::printf("render endpoint  : %s\n", ToUtf8(ren.DeviceName()).c_str());
    std::printf("  mode           : %s\n", rf.exclusive ? "EXCLUSIVE" : "shared");
    std::printf("  format         : %u Hz, %u ch\n", rf.sampleRate, rf.channels);
    std::printf("  device period  : %.2f ms\n", rf.periodMs);
    std::printf("  buffer depth   : %.2f ms (%u frames)\n", rf.bufferMs, rf.bufferFrames);
    std::printf("  queued-to-DAC  : avg %.2f ms, max %.2f ms (IAudioClock estimate)\n",
                queuedAvgMs, queuedMaxMs);
    std::printf("pipeline ring    : capacity %.2f ms, avg depth %.2f ms, max %.2f ms, overruns %llu\n",
                ring.Capacity() * 1000.0 / cf.sampleRate, ringAvgMs, ringMaxMs,
                (unsigned long long)ring.Overruns());
    double endToEnd = capBufMs + ringAvgMs + queuedAvgMs + rf.periodMs;
    std::printf("estimated end-to-end (capture buffer + ring + render queue + render period): %.2f ms\n",
                endToEnd);
    return 0;
}

int RunMeasureLoopback() {
    ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }

    // SoundRadar render endpoint (the speaker side; NOT the loopback).
    Microsoft::WRL::ComPtr<IMMDevice> spk;
    for (const DeviceInfo& d : EnumerateEndpoints(eRender)) {
        std::wstring low = d.name;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low.find(L"soundradar") != std::wstring::npos &&
            low.find(L"loopback") == std::wstring::npos) {
            spk = FindEndpointByName(eRender, d.name);
            break;
        }
    }
    if (!spk) {
        std::printf("SoundRadar render endpoint not found - is the driver installed? Skipping.\n");
        return 2;
    }

    CaptureClient cap;
    std::wstring err;
    if (!cap.Init(err)) {
        std::printf("SoundRadar loopback capture endpoint not found - driver absent? Skipping.\n");
        return 2;
    }

    // Shared-mode render client on the SoundRadar speaker endpoint.
    Microsoft::WRL::ComPtr<IAudioClient> client;
    if (FAILED(spk->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        fwprintf(stderr, L"cannot activate SoundRadar render endpoint\n");
        return 1;
    }
    WAVEFORMATEX* mix = nullptr;
    client->GetMixFormat(&mix);
    PcmFormat pf = InspectFormat(mix);
    if (!pf.valid) {
        std::fprintf(stderr, "unsupported SoundRadar render format: %s\n",
                     ToUtf8(DescribeFormat(mix)).c_str());
        CoTaskMemFree(mix);
        return 1;
    }
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr))) {
        fwprintf(stderr, L"SoundRadar render Initialize failed\n");
        CoTaskMemFree(mix);
        return 1;
    }
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    Microsoft::WRL::ComPtr<IAudioClock> clock;
    client->GetService(IID_PPV_ARGS(&render));
    client->GetService(IID_PPV_ARGS(&clock));
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    // Click train parameters (device rate).
    const uint32_t rate = pf.sampleRate;
    const int kClicks = 8;
    const uint32_t clickLen = rate / 500;        // 2 ms burst
    const uint32_t clickGap = rate / 4;          // one click every 250 ms
    const uint32_t totalFrames = clickGap * (kClicks + 3);

    LONGLONG qf = 0;
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qf));

    // Capture thread records click onsets with QPC timestamps.
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex onsetMu;
    std::vector<LONGLONG> onsets;
    std::thread capThread([&] {
        bool quiet = true;
        cap.Run([&](const float* frames, uint32_t n, LONGLONG qpcPos) {
            for (uint32_t i = 0; i < n; ++i) {
                float m = 0.0f;
                for (int c = 0; c < kChannels; ++c)
                    m = std::max(m, std::fabs(frames[i * kChannels + c]));
                if (quiet && m > 0.3f) {
                    quiet = false;
                    LONGLONG q = qpcPos + (LONGLONG)((double)i / rate * (double)qf);
                    std::lock_guard<std::mutex> lk(onsetMu);
                    onsets.push_back(q);
                } else if (!quiet && m < 0.05f) {
                    quiet = true;
                }
            }
        }, quit);
    });

    // Generate + submit clicks; record estimated play QPC for each click.
    std::vector<LONGLONG> playQpc(kClicks, 0);
    std::vector<float> gen(static_cast<size_t>(bufferFrames) * pf.channels, 0.0f);
    uint64_t produced = 0;   // device frames generated so far
    uint64_t submitted = 0;  // device frames submitted so far
    int clicksScheduled = 0;

    auto fillChunk = [&](uint32_t frames, float* dst) {
        std::memset(dst, 0, static_cast<size_t>(frames) * pf.channels * sizeof(float));
        for (uint32_t f = 0; f < frames; ++f) {
            uint64_t absPos = produced + f;
            uint32_t inClick = static_cast<uint32_t>(absPos % clickGap);
            if (absPos < totalFrames && inClick < clickLen &&
                clicksScheduled <= kClicks) {
                // 1 kHz burst at 0.8 amplitude on FL
                double t = static_cast<double>(inClick) / rate;
                dst[static_cast<size_t>(f) * pf.channels] =
                    static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t));
            }
        }
    };

    // Prime with silence, then start.
    {
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) {
            std::memset(data, 0, static_cast<size_t>(bufferFrames) * pf.blockAlign);
            render->ReleaseBuffer(bufferFrames, 0);
            submitted += bufferFrames;
        }
    }
    client->Start();

    while (produced < totalFrames) {
        Sleep(20);
        UINT32 pad = 0;
        client->GetCurrentPadding(&pad);
        UINT32 avail = bufferFrames - pad;
        if (avail == 0) continue;
        if (avail > gen.size() / pf.channels) avail = static_cast<UINT32>(gen.size() / pf.channels);

        // Which click (if any) starts inside this chunk?
        int clickInChunk = -1;
        uint32_t clickOffset = 0;
        for (int c = 0; c < kClicks; ++c) {
            uint64_t start = static_cast<uint64_t>(c) * clickGap;
            if (start >= produced && start < produced + avail) {
                clickInChunk = c;
                clickOffset = static_cast<uint32_t>(start - produced);
                break;
            }
        }

        fillChunk(avail, gen.data());
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) break;
        ConvertFromFloat(gen.data(), avail, pf, data);
        render->ReleaseBuffer(avail, 0);

        if (clickInChunk >= 0) {
            // Estimate when the click's first frame hits the DAC.
            UINT64 pos = 0, qpc = 0;
            LONGLONG now = 0;
            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now));
            if (clock && SUCCEEDED(clock->GetPosition(&pos, &qpc))) {
                double framesAhead =
                    static_cast<double>(submitted + clickOffset) - static_cast<double>(pos);
                if (framesAhead < 0) framesAhead = 0;
                playQpc[clickInChunk] = now + (LONGLONG)(framesAhead / rate * (double)qf);
            } else {
                playQpc[clickInChunk] = now; // fallback: submit time
            }
            ++clicksScheduled;
        }
        produced += avail;
        submitted += avail;
    }

    // Wait for trailing clicks to arrive, then stop capture.
    Sleep(600);
    SetEvent(quit);
    capThread.join();
    client->Stop();
    CloseHandle(quit);
    CoTaskMemFree(mix);

    std::printf("\n--- SoundRadar loopback round-trip (click train) ---\n");
    std::printf("render format    : %u Hz, %u ch; buffer %u frames (%.2f ms)\n",
                rate, pf.channels, bufferFrames, bufferFrames * 1000.0 / rate);
    std::printf("capture period   : %.2f ms; buffer %u frames (%.2f ms)\n",
                cap.GetFormat().periodMs, cap.GetFormat().bufferFrames,
                cap.GetFormat().bufferFrames * 1000.0 / cap.GetFormat().sampleRate);

    std::vector<LONGLONG> os;
    {
        std::lock_guard<std::mutex> lk(onsetMu);
        os = onsets;
    }
    if (os.empty()) {
        std::printf("no clicks detected in the loopback stream - driver not passing audio?\n");
        return 1;
    }
    int n = (int)std::min(os.size(), (size_t)kClicks);
    double sum = 0, mn = 1e9, mx = 0;
    for (int i = 0; i < n; ++i) {
        double ms = QpcToSec(os[i] - playQpc[i], qf) * 1000.0;
        std::printf("click %d: round-trip %.2f ms\n", i, ms);
        sum += ms;
        mn = std::min(mn, ms);
        mx = std::max(mx, ms);
    }
    std::printf("driver ring round-trip: avg %.2f ms, min %.2f ms, max %.2f ms (%d/%d clicks)\n",
                sum / n, mn, mx, n, kClicks);
    std::printf("note: engine adds capture packet (%.1f ms) + ring depth + render buffer on top;\n",
                cap.GetFormat().periodMs);
    std::printf("      run --measure for the full pipeline estimate on the real output.\n");
    return 0;
}

} // namespace sr
