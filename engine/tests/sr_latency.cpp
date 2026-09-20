// sr_latency.cpp - host harness for the field report "the pointer lags the
// sound and does not leave after the sound stops".
//
// It drives the real Analyzer (engine/analysis.cpp) and the real ArrowTracker
// (overlay/arrow_tracker.h) with synthetic 10 ms WASAPI blocks, and prints the
// timings the report is about:
//   * onset  -> first drawn arrow,
//   * source move -> the arrow reaches the new bearing,
//   * sound stop -> empty scene,
//   * arrows during ambience alone,
//   * the displayed level of a step that sits just above the gate.
//
// Build and run:  sh engine/tests/run_latency.sh
#include "../analysis.h"
#include "../../overlay/arrow_tracker.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using sr::AnalysisConfig;
using sr::AnalysisFrame;
using sr::Analyzer;
using sr::ArrowTracker;

// The overlay settings the overlay thread hands to ArrowTracker. Copied from
// OverlayConfig in engine/config.h, because that header needs windows.h.
struct OverlayCfg {
    float detectThreshold = 0.005f;
    int arrowFadeMs = 500;
    bool frontMerge = true;
};

namespace {

constexpr double kFs = 48000.0;
constexpr size_t kCh = 8;
constexpr double kBlockMs = 10.0;

uint32_t s_rng = 0x9e3779b9u;
float Rand11() {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return static_cast<float>(s_rng & 0xFFFFFF) / 8388608.0f - 1.0f;
}

// One 10 ms block: white ambience plus zero or more (channel, amplitude) bursts.
struct BlockSpec {
    double ambience = 0.0012; // ~ -58 dBFS
    std::vector<std::pair<int, double>> bursts;
};

struct Sample {
    double ms = 0;
    float level[8] = {};
    bool active = false;
    bool gate = false;
    int arrows = 0;
    float angle = 0;
    bool present = false;
};

struct Scene {
    std::vector<BlockSpec> blocks;
};

std::vector<Sample> Run(const Scene& scene, const AnalysisConfig& acfg,
                        const OverlayCfg& ocfg) {
    Analyzer an(acfg);
    ArrowTracker tr;
    std::vector<Sample> out;
    double ms = 0;
    const size_t frames = static_cast<size_t>(kFs * kBlockMs / 1000.0);
    std::vector<float> pcm(frames * kCh);
    for (const auto& bs : scene.blocks) {
        for (size_t f = 0; f < frames; ++f) {
            for (size_t c = 0; c < kCh; ++c) {
                float v = static_cast<float>(Rand11() * bs.ambience);
                for (const auto& bu : bs.bursts)
                    if (static_cast<size_t>(bu.first) == c)
                        v += static_cast<float>(bu.second);
                pcm[f * kCh + c] = v;
            }
        }
        AnalysisFrame fr;
        an.Process(pcm.data(), frames, fr);
        float lv[8];
        for (int c = 0; c < 8; ++c) lv[c] = fr.level[c];
        const bool active = fr.active;
        float zeros[8] = {};
        tr.Update(active ? lv : zeros, ocfg.detectThreshold,
                  static_cast<float>(kBlockMs / 1000.0), ocfg.frontMerge,
                  ocfg.arrowFadeMs);
        if (!active) tr.Clear();
        Sample s;
        s.ms = ms + kBlockMs * 0.5;
        for (int c = 0; c < 8; ++c) s.level[c] = fr.level[c];
        s.active = active;
        s.gate = fr.gateOpen;
        const auto& ar = tr.Arrows();
        s.arrows = static_cast<int>(ar.size());
        if (!ar.empty()) {
            s.angle = ar[0].angle;
            s.present = true;
        }
        out.push_back(s);
        ms += kBlockMs;
    }
    return out;
}

template <typename F>
double FirstAt(const std::vector<Sample>& s, F pred) {
    for (const auto& x : s)
        if (pred(x)) return x.ms;
    return -1.0;
}

void SceneQuiet(Scene& sc, int blocks, double amb = 0.0012) {
    for (int i = 0; i < blocks; ++i) {
        BlockSpec b;
        b.ambience = amb;
        sc.blocks.push_back(b);
    }
}

// An exponentially decaying thump, like a footstep: loud for about 60 ms.
void SceneStep(Scene& sc, int ch, double amp, int blocks, double amb = 0.0012) {
    for (int i = 0; i < blocks; ++i) {
        BlockSpec b;
        b.ambience = amb;
        b.bursts.push_back({ch, amp * std::exp(-i * 0.5)});
        sc.blocks.push_back(b);
    }
}

void SceneTone(Scene& sc, int ch, double amp, int blocks, double amb = 0.0012) {
    for (int i = 0; i < blocks; ++i) {
        BlockSpec b;
        b.ambience = amb;
        b.bursts.push_back({ch, amp});
        sc.blocks.push_back(b);
    }
}

void Dump(const char* title, const std::vector<Sample>& s, double from, double to) {
    std::printf("\n-- %s --\n", title);
    std::printf("     ms | gate act | arrows | angle | FL     FR     BL     SL\n");
    for (const auto& x : s) {
        if (x.ms < from || x.ms > to) continue;
        std::printf("  %5.0f |  %d   %d   |   %d    | %6.1f | %5.2f  %5.2f  %5.2f  %5.2f\n",
                    x.ms, (int)x.gate, (int)x.active, x.arrows, x.angle, x.level[0],
                    x.level[1], x.level[4], x.level[6]);
    }
}

// Pass/fail counters, in the style of sr_probe.cpp.
int g_fail = 0;

void Check(bool ok, const char* what, const std::string& detail) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "  - ", detail.c_str());
    if (!ok) ++g_fail;
}

} // namespace

int main() {
    AnalysisConfig acfg;   // shipped defaults from analysis.h
    if (const char* e = getenv("SR_LEGACY")) if (*e) acfg.detectAdaptive = false;
    OverlayCfg ocfg;       // shipped overlay defaults

    std::printf("analysis defaults: attack %.0f ms release %.0f ms fade %d ms "
                "smooth %.0f ms activity %.3f gateMargin %.0f dB hold %d ms hang %d ms\n",
                acfg.attackMs, acfg.releaseMs, acfg.fadeMs, acfg.smoothMs,
                acfg.activityThreshold, acfg.gateMarginDb, acfg.gateHoldMs,
                acfg.gateHangMs);
    std::printf("overlay defaults: detectThreshold %.4f arrowFadeMs %d frontMerge %d\n",
                ocfg.detectThreshold, ocfg.arrowFadeMs, (int)ocfg.frontMerge);
    std::printf("scenario: 10 ms blocks, ambience -58 dBFS, frontMerge on\n");

    // ---- 1. ambience alone, 3 s, several ambient levels -------------------
    std::printf("\n== 1. ambience alone (no sound at all) ==\n");
    for (double amb : {0.0006, 0.0012, 0.0025, 0.005, 0.010}) {
        Scene sc;
        SceneQuiet(sc, 300, amb);
        auto s = Run(sc, acfg, ocfg);
        double maxLvl = 0;
        int arrowBlocks = 0;
        for (const auto& x : s) {
            for (int c = 0; c < 8; ++c) maxLvl = std::max<double>(maxLvl, x.level[c]);
            if (x.present) ++arrowBlocks;
        }
        std::printf("   ambience %6.1f dBFS | max display level %.3f | arrow blocks %d/300\n",
                    20.0 * std::log10(amb), maxLvl, arrowBlocks);
        char det[128];
        std::snprintf(det, sizeof(det), "ambience %.0f dBFS, max level %.3f, %d blocks with an arrow",
                      20.0 * std::log10(amb), maxLvl, arrowBlocks);
        Check(arrowBlocks == 0 && maxLvl < ocfg.detectThreshold, "ambience draws no arrow", det);
    }

    // ---- 2. one footstep, onset and tail ---------------------------------
    {
        Scene sc;
        SceneQuiet(sc, 40);
        SceneStep(sc, 0, 0.20, 12);   // FL at 400 ms
        SceneQuiet(sc, 60);
        auto s = Run(sc, acfg, ocfg);
        const double onset = 400.0;
        const double stop = 400.0 + 12 * kBlockMs; // 520 ms: the thump is over
        const double firstArrow = FirstAt(s, [](const Sample& x) { return x.present; });
        const double gone = FirstAt(s, [&](const Sample& x) {
            return x.ms > stop && !x.present;
        });
        std::printf("\n== 2. one FL footstep at 400 ms ==\n");
        std::printf("   onset %.0f -> first arrow %.0f ms (%.0f ms late)\n", onset,
                    firstArrow, firstArrow - onset);
        std::printf("   thump ends %.0f -> scene empty %.0f ms (%.0f ms tail)\n", stop,
                    gone, gone - stop);
        Dump("trace", s, 380, 700);
    }

    // ---- 3. a held quiet tone just above the gate -------------------------
    {
        Scene sc;
        SceneQuiet(sc, 40);
        SceneTone(sc, 6, 0.02, 40);   // SL, 20 dB below the loud step
        SceneQuiet(sc, 40);
        auto s = Run(sc, acfg, ocfg);
        const double onset = 400.0;
        const double stop = 800.0;
        const double firstArrow = FirstAt(s, [](const Sample& x) { return x.present; });
        const double gone = FirstAt(s, [&](const Sample& x) {
            return x.ms > stop && !x.present;
        });
        std::printf("\n== 3. held quiet tone on SL, 400-800 ms ==\n");
        std::printf("   onset %.0f -> first arrow %.0f ms (%.0f ms late); "
                    "stop %.0f -> empty %.0f ms (%.0f ms tail)\n",
                    onset, firstArrow, firstArrow - onset, stop, gone, gone - stop);
        Dump("trace", s, 380, 1000);
    }

    // ---- 4. direction change: FL -> FR ------------------------------------
    {
        Scene sc;
        SceneQuiet(sc, 40);
        SceneTone(sc, 0, 0.20, 30);   // FL 400-700
        SceneTone(sc, 1, 0.20, 30);   // FR 700-1000
        SceneQuiet(sc, 40);
        auto s = Run(sc, acfg, ocfg);
        const double move = 700.0;
        const double arrived = FirstAt(s, [&](const Sample& x) {
            return x.ms > move && x.present &&
                   std::fabs(sr::ArrowAngDist(x.angle, 30.0f)) < 8.0f;
        });
        std::printf("\n== 4. source moves FL(-30) -> FR(+30) at 700 ms ==\n");
        std::printf("   arrow reaches +30 at %.0f ms (%.0f ms after the move)\n",
                    arrived, arrived - move);
        Dump("trace", s, 640, 1000);
        Check(arrived > 0 && arrived - move <= 100.0,
              "arrow follows a source that steps to the next channel",
              std::to_string(arrived - move) + " ms");
    }

    std::printf("\n%s (%d failure(s))\n", g_fail ? "LATENCY PROBE FAILED" : "ALL LATENCY PROBES PASSED", g_fail);
    return g_fail ? 1 : 0;
}
