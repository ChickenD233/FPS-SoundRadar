// selftest_detect.cpp - self-test for the adaptive detection front end.
// Pure C++, no audio devices. Runs with --sselftest.
//
// It synthesizes the field case: a game mix whose global volume is low, so
// footsteps sit far below the old fixed thresholds. The test proves:
//   1) a quiet footstep train is detected,
//   2) plain noise produces no classification and no display level,
//   3) the display returns to a true zero after sound stops,
//   4) a loud passage does not blind the detector afterwards.
#include "selftest_detect.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "analysis.h"
#include "classify.h"
#include "floatcmp.h"

namespace sr {

namespace {

constexpr size_t kCh = 8;
constexpr double kFs = 48000.0;

int g_fail = 0;

void Report(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

uint32_t g_rng = 0x12345678u;

float Rand11() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return static_cast<float>(g_rng & 0xFFFFFF) / 8388608.0f - 1.0f;
}

double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

// Low-band thump with a ~200 ms tail, like a footstep in a game mix.
void Step(std::vector<float>& pcm, size_t frames, size_t at, double amp) {
    const size_t n = static_cast<size_t>(0.2 * kFs);
    for (size_t i = 0; i < n && at + i < frames; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double env = std::exp(-t / (200.0 * 0.00035));
        const double v = std::sin(2.0 * M_PI * 150.0 * t) * 0.7 +
                         std::sin(2.0 * M_PI * 225.0 * t) * 0.3;
        pcm[(at + i) * kCh] += static_cast<float>(v * env * amp);
    }
}

void NoiseFloor(std::vector<float>& pcm, double db) {
    const float a = static_cast<float>(DbToLin(db));
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] += Rand11() * a;
}

struct Result {
    bool footstep = false;
    float peakLevel = 0.0f;
    int zeroAtMs = -1;
};

Result RunPipeline(const std::vector<float>& pcm, size_t frames, const AnalysisConfig& acfg,
                   const ClassifyConfig& ccfg) {
    Analyzer an(acfg);
    Classifier8 clf(ccfg);
    Result r;
    AnalysisFrame fr{};
    const size_t block = 480;
    for (size_t off = 0; off + block <= frames; off += block) {
        an.Process(pcm.data() + off * kCh, block, fr);
        clf.Process(pcm.data() + off * kCh, block, fr.detectGain, fr.gateOpen,
                    fr.detectThreshold, an.BurstThresholds());
        if (clf.ClassOf(0) == SoundFootstep) r.footstep = true;
        if (FCmpGt(fr.level[0], r.peakLevel)) r.peakLevel = fr.level[0];
        // A true zero, only after the channel was lit.
        if (r.zeroAtMs < 0 && FCmpGt(r.peakLevel, 0.1f) && FCmpLe(fr.level[0], 0.0f))
            r.zeroAtMs = static_cast<int>(off / 48);
    }
    return r;
}

// 1) Quiet mix: a step train 20 dB above the ambient floor is detected.
void TestQuietFootsteps() {
    bool ok = true;
    for (double floorDb : { -70.0, -60.0, -50.0 }) {
        std::vector<float> pcm(static_cast<size_t>(1.5 * kFs) * kCh, 0.0f);
        const size_t frames = pcm.size() / kCh;
        NoiseFloor(pcm, floorDb);
        for (int i = 0; i < 3; ++i)
            Step(pcm, frames, static_cast<size_t>((0.3 + 0.25 * i) * kFs), DbToLin(-36.0));
        Result r = RunPipeline(pcm, frames, AnalysisConfig(), ClassifyConfig());
        std::printf("       floor %.0f dBFS -> footstep=%d level=%.3f\n", floorDb,
                    r.footstep ? 1 : 0, r.peakLevel);
        if (!r.footstep) ok = false;
    }
    Report("quiet mix: footsteps 20 dB above the ambient floor are detected", ok);
}

// 2) Noise alone: no classification and no display level.
void TestNoiseOnly() {
    bool ok = true;
    const float margins[] = { -70.0f, -60.0f, -50.0f, -40.0f, -34.0f };
    for (float floorDb : margins) {
        std::vector<float> pcm(static_cast<size_t>(1.5 * kFs) * kCh, 0.0f);
        const size_t frames = pcm.size() / kCh;
        NoiseFloor(pcm, floorDb);
        Result r = RunPipeline(pcm, frames, AnalysisConfig(), ClassifyConfig());
        std::printf("       floor %.0f dBFS -> level=%.4f classified=%d\n", floorDb,
                    r.peakLevel, r.footstep ? 1 : 0);
        if (r.footstep || !FCmpLt(r.peakLevel, 0.02f)) ok = false;
    }
    Report("noise alone: no classification and no visible level", ok);
}

// 3) Fade: the display reaches a true zero after the sound stops.
void TestFadeToZero() {
    std::vector<float> pcm(static_cast<size_t>(1.9 * kFs) * kCh, 0.0f);
    const size_t frames = pcm.size() / kCh;
    Step(pcm, frames, 0, 0.5);
    Step(pcm, frames, static_cast<size_t>(0.6 * kFs), 0.5);
    NoiseFloor(pcm, -70.0); // residual hiss, not digital zero
    Result r = RunPipeline(pcm, frames, AnalysisConfig(), ClassifyConfig());
    const bool lit = FCmpGt(r.peakLevel, 0.1f);
    const bool zeroed = r.zeroAtMs > 500 && r.zeroAtMs <= 1900;
    std::printf("       peak=%.3f zeroAt=%d ms\n", r.peakLevel, r.zeroAtMs);
    Report("silence fade: the level reaches a true zero", lit && zeroed);
}

// 4) Recovery: a loud passage must not blind the detector for long.
void TestRecovery() {
    std::vector<float> pcm(static_cast<size_t>(2.9 * kFs) * kCh, 0.0f);
    const size_t frames = pcm.size() / kCh;
    const size_t loudEnd = static_cast<size_t>(1.0 * kFs) * kCh;
    for (size_t i = 0; i < loudEnd; ++i) pcm[i] = Rand11() * static_cast<float>(DbToLin(-22.0));
    for (size_t i = loudEnd; i < pcm.size(); ++i) pcm[i] = Rand11() * static_cast<float>(DbToLin(-70.0));
    for (int i = 0; i < 3; ++i)
        Step(pcm, frames, static_cast<size_t>((2.0 + 0.25 * i) * kFs), DbToLin(-42.0));
    Result r = RunPipeline(pcm, frames, AnalysisConfig(), ClassifyConfig());
    std::printf("       after loud ambience -> footstep=%d level=%.3f\n", r.footstep ? 1 : 0,
                r.peakLevel);
    Report("recovery: quiet steps after a loud passage are detected", r.footstep);
}

} // namespace

int RunDetectSelfTest() {
    std::printf("SoundRadar adaptive detection self-test (no audio devices)\n\n");
    g_fail = 0;
    TestQuietFootsteps();
    TestNoiseOnly();
    TestFadeToZero();
    TestRecovery();
    std::printf("\n%s (%d failure%s)\n",
                g_fail == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}

} // namespace sr
