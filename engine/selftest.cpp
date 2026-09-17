// selftest.cpp - proves: no cross-channel mixing, channel independence,
// right-mono weighted sum correctness, silence fade timing, config round-trip.
#include "selftest.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "analysis.h"
#include "config.h"
#include "downmix.h"

namespace sr {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

int g_failures = 0;

void Report(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Sine amplitude estimate at `freq` via Goertzel (no FFT library needed).
double GoertzelAmp(const float* x, size_t n, double freq) {
    double w = 2.0 * kPi * freq / kFs;
    double cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double s0 = static_cast<double>(x[i]) + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 2.0 * std::sqrt(power < 0 ? 0 : power) / static_cast<double>(n);
}

// Fills `frames` frames of interleaved 8ch; sine at `freq`/`amp` in channel `ch` only.
void SynthSingle(std::vector<float>& buf, size_t frames, int ch, double freq, double amp,
                 size_t startSample = 0) {
    buf.assign(frames * kChannels, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        double t = static_cast<double>(f + startSample) / kFs;
        buf[f * kChannels + ch] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * t));
    }
}

// Extracts one output channel (0=L, 1=R) from interleaved stereo.
std::vector<float> ChannelOf(const std::vector<float>& stereo, int ch) {
    std::vector<float> out(stereo.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = stereo[i * 2 + ch];
    return out;
}

// --- tests ------------------------------------------------------------------

// 1) A sine in exactly one channel must light up only that channel's meter.
void TestChannelIsolation() {
    bool ok = true;
    for (int ch = 0; ch < kChannels && ok; ++ch) {
        Analyzer an;
        std::vector<float> buf;
        SynthSingle(buf, 12000, ch, 300.0 + 100.0 * ch, 1.0); // 250 ms
        AnalysisFrame fr{};
        // Feed in 10 ms blocks like the real pipeline.
        for (size_t off = 0; off < buf.size() / kChannels; off += 480) {
            an.Process(buf.data() + off * kChannels, 480, fr);
        }
        if (!(fr.level[ch] > 0.5f && fr.peak[ch])) ok = false;
        for (int c = 0; c < kChannels; ++c) {
            if (c == ch) continue;
            if (!(fr.level[c] < 0.05f && !fr.peak[c])) ok = false;
        }
        if (!ok)
            std::printf("       channel %d: level=%.3f (want >0.5), max other=%.3f\n",
                        ch, fr.level[ch], [&] {
                            float m = 0;
                            for (int c = 0; c < kChannels; ++c)
                                if (c != ch && fr.level[c] > m) m = fr.level[c];
                            return m;
                        }());
    }
    Report("channel isolation (no cross-channel mixing)", ok);
}

// 2) FL + BR simultaneously: exactly those two meters high (no averaging to center).
void TestChannelIndependence() {
    Analyzer an;
    const size_t frames = 12000;
    std::vector<float> buf(frames * kChannels, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        double t = static_cast<double>(f) / kFs;
        buf[f * kChannels + 0] = static_cast<float>(std::sin(2.0 * kPi * 500.0 * t)); // FL
        buf[f * kChannels + 5] = static_cast<float>(std::sin(2.0 * kPi * 900.0 * t)); // BR
    }
    AnalysisFrame fr{};
    for (size_t off = 0; off < frames; off += 480)
        an.Process(buf.data() + off * kChannels, 480, fr);

    bool ok = fr.level[0] > 0.5f && fr.level[5] > 0.5f && fr.peak[0] && fr.peak[5];
    for (int c = 1; c < kChannels; ++c) {
        if (c == 5) continue;
        if (!(fr.level[c] < 0.05f && !fr.peak[c])) ok = false;
    }
    if (!ok)
        std::printf("       FL=%.3f BR=%.3f C=%.3f (C must stay low)\n",
                    fr.level[0], fr.level[5], fr.level[2]);
    Report("channel independence (FL+BR, no center merge)", ok);
}

// 3) RIGHT_MONO: right out = weighted sum (per channel), left out = 0.
void TestRightMonoPerChannel() {
    DownmixConfig cfg; // default weights
    cfg.mode = DownmixRightMono;
    bool ok = true;
    const size_t frames = 4800; // 100 ms, Goertzel bin width 10 Hz
    for (int ch = 0; ch < kChannels; ++ch) {
        double freq = 300.0 + 100.0 * ch;
        double amp = 0.2;
        std::vector<float> in, out(frames * 2);
        SynthSingle(in, frames, ch, freq, amp);
        Downmix8To2(in.data(), out.data(), frames, cfg);
        std::vector<float> right = ChannelOf(out, 1);
        std::vector<float> left = ChannelOf(out, 0);
        double got = GoertzelAmp(right.data(), frames, freq);
        double want = amp * cfg.weights[ch];
        double lmax = 0.0;
        for (float v : left) lmax = std::max(lmax, static_cast<double>(std::fabs(v)));
        bool chOk = std::fabs(got - want) / want < 0.03 && lmax == 0.0;
        if (!chOk) {
            std::printf("       ch %d: right amp %.4f (want %.4f), left max %.6f\n",
                        ch, got, want, lmax);
            ok = false;
        }
    }
    Report("right-mono weighted sum per channel", ok);
}

// 4) 8 distinct-frequency sines, one per channel: all present in right output.
void TestRightMonoAllChannelsPresent() {
    DownmixConfig cfg;
    cfg.mode = DownmixRightMono;
    const size_t frames = 4800;
    const double amp = 0.05; // small enough that the tanh soft-clip stays ~linear
    std::vector<float> in(frames * kChannels, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        double t = static_cast<double>(f) / kFs;
        for (int ch = 0; ch < kChannels; ++ch) {
            double freq = 300.0 + 100.0 * ch;
            in[f * kChannels + ch] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * t));
        }
    }
    std::vector<float> out(frames * 2);
    Downmix8To2(in.data(), out.data(), frames, cfg);
    std::vector<float> right = ChannelOf(out, 1);

    bool ok = true;
    for (int ch = 0; ch < kChannels; ++ch) {
        double freq = 300.0 + 100.0 * ch;
        double got = GoertzelAmp(right.data(), frames, freq);
        double want = amp * cfg.weights[ch];
        if (std::fabs(got - want) / want > 0.08) {
            std::printf("       %.0f Hz: amp %.4f (want %.4f)\n", freq, got, want);
            ok = false;
        }
    }
    double absent = GoertzelAmp(right.data(), frames, 2000.0); // not in the mix
    if (absent > 0.005) {
        std::printf("       2000 Hz (absent): amp %.4f\n", absent);
        ok = false;
    }
    Report("right-mono carries all 8 channels (Goertzel)", ok);
}

// 5) Silence fade: level must decay to <0.05 within fadeMs+100, not before fadeMs-100.
void TestFadeTiming() {
    AnalysisConfig acfg;
    acfg.fadeMs = 400;
    Analyzer an(acfg);

    std::vector<float> burst;
    SynthSingle(burst, 14400, 0, 700.0, 0.8); // 300 ms burst on FL
    AnalysisFrame fr{};
    for (size_t off = 0; off < 14400; off += 480)
        an.Process(burst.data() + off * kChannels, 480, fr);
    bool burstOk = fr.level[0] > 0.4f;

    // Silence in 10 ms blocks; find first block where level < 0.05.
    std::vector<float> silence(480 * kChannels, 0.0f);
    int firstBelowMs = -1;
    for (int b = 1; b <= 70; ++b) { // up to 700 ms of silence
        an.Process(silence.data(), 480, fr);
        if (fr.level[0] < 0.05f) { firstBelowMs = b * 10; break; }
    }
    bool ok = burstOk && firstBelowMs >= acfg.fadeMs - 100 && firstBelowMs <= acfg.fadeMs + 100;
    if (!ok)
        std::printf("       burst level ok=%d, first <0.05 at %d ms (want %d..%d)\n",
                    burstOk ? 1 : 0, firstBelowMs, acfg.fadeMs - 100, acfg.fadeMs + 100);
    Report("silence fade timing (~fade_ms)", ok);
}

// 6) Stereo downmix sanity: FL goes left, SR goes right, center splits.
void TestStereoDownmix() {
    DownmixConfig cfg;
    cfg.mode = DownmixStereo;
    const size_t frames = 4800;
    bool ok = true;

    std::vector<float> in, out(frames * 2);
    SynthSingle(in, frames, 0, 500.0, 0.2); // FL only
    Downmix8To2(in.data(), out.data(), frames, cfg);
    std::vector<float> L = ChannelOf(out, 0), R = ChannelOf(out, 1);
    double la = GoertzelAmp(L.data(), frames, 500.0);
    double ra = GoertzelAmp(R.data(), frames, 500.0);
    if (std::fabs(la - 0.2) / 0.2 > 0.05 || ra > 0.001) {
        std::printf("       FL-only: L=%.4f (want ~0.2) R=%.4f (want ~0)\n", la, ra);
        ok = false;
    }

    SynthSingle(in, frames, 2, 700.0, 0.2); // C only -> both sides at -3 dB
    Downmix8To2(in.data(), out.data(), frames, cfg);
    L = ChannelOf(out, 0); R = ChannelOf(out, 1);
    la = GoertzelAmp(L.data(), frames, 700.0);
    ra = GoertzelAmp(R.data(), frames, 700.0);
    double want = 0.2 * 0.7071;
    if (std::fabs(la - want) / want > 0.05 || std::fabs(ra - want) / want > 0.05) {
        std::printf("       C-only: L=%.4f R=%.4f (want ~%.4f each)\n", la, ra, want);
        ok = false;
    }
    Report("stereo 7.1->2.0 downmix (ITU)", ok);
}

// 7) Config round-trip: save + load preserves values.
void TestConfigRoundTrip() {
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring path = (n > 0 ? std::wstring(tmp) : L".") + L"soundradar_selftest_config.json";

    AppConfig a;
    a.downmix.mode = DownmixStereo;
    for (int i = 0; i < kChannels; ++i) a.downmix.weights[i] = 0.1f * (i + 1);
    a.analysis.fadeMs = 350;
    a.analysis.activityThreshold = 0.07f;
    a.outputDevice = L"TestDevice";
    a.overlay.enabled = false;
    a.overlay.highThreshold = 0.6f;
    a.overlay.radius = 120;
    a.autostart = true;

    bool ok = SaveConfig(path, a);
    AppConfig b;
    ok = ok && LoadConfig(path, b);
    ok = ok && b.downmix.mode == DownmixStereo;
    for (int i = 0; i < kChannels; ++i)
        ok = ok && std::fabs(b.downmix.weights[i] - a.downmix.weights[i]) < 0.002f;
    ok = ok && b.analysis.fadeMs == 350;
    ok = ok && std::fabs(b.analysis.activityThreshold - 0.07f) < 1e-4f;
    ok = ok && b.outputDevice == L"TestDevice";
    ok = ok && b.overlay.enabled == false && b.autostart == true;
    ok = ok && std::fabs(b.overlay.highThreshold - 0.6f) < 1e-4f && b.overlay.radius == 120;
    DeleteFileW(path.c_str());
    Report("config save/load round-trip", ok);
}

} // namespace

int RunSelfTest() {
    std::printf("SoundRadar engine self-test (no audio devices required)\n\n");
    TestChannelIsolation();
    TestChannelIndependence();
    TestRightMonoPerChannel();
    TestRightMonoAllChannelsPresent();
    TestFadeTiming();
    TestStereoDownmix();
    TestConfigRoundTrip();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

} // namespace sr
