// sr_probe.cpp - host-side (macOS/Linux) measurement harness for the pure
// detection path (Analyzer + Classifier8). No Windows APIs, no audio devices.
//
// It synthesizes 8-channel scenarios that match the field reports:
//   * a quiet game mix (global volume 40%) leaves footsteps far below the
//     fixed thresholds,
//   * a silent stretch must decay the display to zero.
//
// Build and run:  ./engine/tests/run_probe.sh
#include "../analysis.h"
#include "../floatcmp.h"
#include "../classify.h"
#include "../downmix.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstring>
#include <string>
#include <vector>

using sr::Analyzer;
using sr::AnalysisConfig;
using sr::AnalysisFrame;
using sr::Classifier8;
using sr::ClassifyConfig;
using sr::SoundClass;
using sr::SoundFootstep;
using sr::SoundGunshot;
using sr::SoundImpact;
using sr::FCmpGt;
using sr::FCmpGe;
using sr::FCmpLt;
using sr::FCmpLe;
using sr::FCmpEq;

namespace {

constexpr size_t kCh = 8;
constexpr double kFs = 48000.0;

struct Buf {
    std::vector<float> pcm; // interleaved, 8 ch
    size_t frames = 0;
};

double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

// xorshift32: deterministic across platforms.
uint32_t s_rng = 0x12345678u;
float Rand11() {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return static_cast<float>(s_rng & 0xFFFFFF) / 8388608.0f - 1.0f;
}

// Low-band thump (footstep): 120-180 Hz partials with a fast exp decay.
void Burst(Buf& b, int ch, size_t at, double amp, double ms, double baseHz) {
    size_t n = static_cast<size_t>(ms * kFs / 1000.0);
    for (size_t i = 0; i < n && at + i < b.frames; ++i) {
        double t = static_cast<double>(i) / kFs;
        double envv = std::exp(-t / (ms * 0.00035));
        double v = std::sin(2.0 * M_PI * baseHz * t) * 0.7 +
                   std::sin(2.0 * M_PI * (baseHz * 1.5) * t) * 0.3;
        b.pcm[(at + i) * kCh + ch] += static_cast<float>(v * envv * amp);
    }
}

// Broadband crack (gunshot / impact): noise burst, optional low thump.
void Crack(Buf& b, int ch, size_t at, double amp, double ms, double thumpAmp) {
    size_t n = static_cast<size_t>(ms * kFs / 1000.0);
    for (size_t i = 0; i < n && at + i < b.frames; ++i) {
        double t = static_cast<double>(i) / kFs;
        double envv = std::exp(-t / (ms * 0.0004));
        double v = Rand11() * envv;
        if (FCmpGt(static_cast<float>(thumpAmp), 0.0f))
            v += thumpAmp * std::sin(2.0 * M_PI * 150.0 * t) * envv;
        b.pcm[(at + i) * kCh + ch] += static_cast<float>(v * amp);
    }
}

void FloorNoise(Buf& b, double db) {
    float a = static_cast<float>(DbToLin(db));
    for (size_t i = 0; i < b.frames * kCh; ++i) b.pcm[i] += Rand11() * a;
}

Buf MakeBuf(double seconds) {
    Buf b;
    b.frames = static_cast<size_t>(seconds * kFs);
    b.pcm.assign(b.frames * kCh, 0.0f);
    return b;
}

struct Obs {
    bool step[8] = {};
    bool gun[8] = {};
    bool impact[8] = {};
};

// Classifier-side config: no display validation hold, so the classification
// test sees every block the raw gate opens on.
AnalysisConfig ClsCfg() {
    AnalysisConfig c;
    c.gateHoldMs = 0;
    return c;
}

// Full pipeline: Analyzer (adaptive gain + gate) feeds Classifier8, exactly as
// the capture thread does it. Records classes, peak level, and peak gain.
struct Run {
    Obs obs;
    int gateBlocks = 0;
    float peakLevel = 0.0f;
    float peakGain = 1.0f;
    float minFloor = 0.0f;
    int zeroAtMs = -1; // first block where the channel reached level 0
};

Run RunPipeline(const Buf& b, const AnalysisConfig& acfg, const ClassifyConfig& ccfg,
                bool trace = false) {
    Analyzer an(acfg);
    Classifier8 clf(ccfg);
    Run r;
    AnalysisFrame fr{};
    const size_t block = 480;
    int bi = 0;
    for (size_t off = 0; off + block <= b.frames; off += block, ++bi) {
        an.Process(b.pcm.data() + off * kCh, block, fr);
        clf.Process(b.pcm.data() + off * kCh, block, fr.detectGain, fr.gateOpen,
                    fr.detectThreshold, an.BurstThresholds());
        for (int c = 0; c < 8; ++c) {
            SoundClass s = clf.ClassOf(c);
            if (s == SoundFootstep) r.obs.step[c] = true;
            if (s == SoundGunshot) r.obs.gun[c] = true;
            if (s == SoundImpact) r.obs.impact[c] = true;
        }
        if (fr.gateOpen) ++r.gateBlocks;
        if (trace && bi < 140)
            std::printf("      blk %2d t=%4dms lvl=%.4f gain=%5.1f floor=%7.2f gate=%d cls=%d\n",
                        bi, bi * 10, fr.level[0], fr.detectGain, fr.noiseFloorDbfs,
                        fr.gateOpen ? 1 : 0, static_cast<int>(clf.ClassOf(0)));
        if (FCmpGt(fr.level[0], r.peakLevel)) r.peakLevel = fr.level[0];
        if (FCmpGt(fr.detectGain, r.peakGain)) r.peakGain = fr.detectGain;
        if (FCmpEq(r.minFloor, 0.0f) || FCmpLt(fr.noiseFloorDbfs, r.minFloor))
            r.minFloor = fr.noiseFloorDbfs;
        // first return to zero AFTER the channel was lit (the warmup blocks
        // report zero by design)
        if (r.zeroAtMs < 0 && FCmpGt(r.peakLevel, 0.1f) && FCmpLe(fr.level[0], 0.0f))
            r.zeroAtMs = static_cast<int>(off / 48);
    }
    return r;
}

std::string ClassesOf(const Obs& o) {
    std::string s;
    for (int c = 0; c < 8; ++c) {
        if (o.step[c]) s += "step" + std::to_string(c) + " ";
        if (o.gun[c]) s += "gun" + std::to_string(c) + " ";
        if (o.impact[c]) s += "impact" + std::to_string(c) + " ";
    }
    if (s.empty()) s = "-";
    return s;
}

int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_fail;
}

} // namespace

int main(int argc, char** argv) {
    bool dump = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--dump") dump = true;

    // ---------------------------------------------------------------- 1) level
    std::printf("== 1. quiet-mix sensitivity (footstep detection) ==\n");
    std::printf("   noisefloor   step amp |  legacy fixed   |  adaptive (new)\n");
    for (double noiseDb : { -80.0, -60.0, -50.0 }) {
        for (double stepDb : { -24.0, -30.0, -36.0, -42.0, -48.0 }) {
            Buf b = MakeBuf(2.0);
            FloorNoise(b, noiseDb);
            for (int i = 0; i < 3; ++i)
                // a real step is a low-band thump with a ~200 ms tail
                Burst(b, 0, static_cast<size_t>((0.3 + 0.25 * i) * kFs),
                      DbToLin(stepDb), 200.0, 150.0);
            AnalysisConfig legacyCfg;
            legacyCfg.detectAdaptive = false;
            Run legacy = RunPipeline(b, legacyCfg, ClassifyConfig());
            // separate window from the classifier: the arrow path has no hold
            Run now = RunPipeline(b, ClsCfg(), ClassifyConfig(),
                                  dump && noiseDb == -60.0 && stepDb == -24.0);
            std::printf("   %6.1f dB   %6.1f dB |  %-12s  |  %-12s  (lvl %.3f gain %.1fx)\n",
                        noiseDb, stepDb, legacy.obs.step[0] ? "FOOTSTEP" : "-",
                        now.obs.step[0] ? "FOOTSTEP" : "-", now.peakLevel, now.peakGain);
        }
    }

    // ------------------------------------------------------------ 2) false pos
    std::printf("\n== 2. noise alone must not classify or light the overlay ==\n");
    for (double noiseDb : { -70.0, -60.0, -50.0, -40.0, -34.0 }) {
            Buf b = MakeBuf(1.5);
        FloorNoise(b, noiseDb);
        Run r = RunPipeline(b, AnalysisConfig(), ClassifyConfig());
        Check(!r.obs.step[0] && !r.obs.gun[0] && !r.obs.impact[0] && FCmpLt(r.peakLevel, 0.02f),
              ("noise " + std::to_string(static_cast<int>(noiseDb)) + " dBFS -> quiet").c_str(),
              ClassesOf(r.obs) + " lvl " + std::to_string(r.peakLevel) +
                  " gateBlocks " + std::to_string(r.gateBlocks));
    }

    // ------------------------------------------------------------- 3) fade-out
    std::printf("\n== 3. display decays to a true zero after sound stops ==\n");
    {
        // Burst, 0.3 s of silence, another burst, then a quiet tail with hiss.
        Buf loud = MakeBuf(0.9);
        Burst(loud, 0, 0, 0.5, 200.0, 150.0);
        Burst(loud, 0, static_cast<size_t>(0.6 * kFs), 0.5, 200.0, 150.0);
        Buf quiet = MakeBuf(1.0);
        FloorNoise(quiet, -70.0); // residual hiss, not digital zero
        Buf b = loud;
        b.pcm.insert(b.pcm.end(), quiet.pcm.begin(), quiet.pcm.end());
        b.frames += quiet.frames;
        Run r = RunPipeline(b, ClsCfg(), ClassifyConfig());
        Check(FCmpGt(r.peakLevel, 0.1f), "burst still lights the channel",
              "peak " + std::to_string(r.peakLevel));
        Check(r.zeroAtMs >= 500 && r.zeroAtMs <= 1500,
              "level reaches exactly 0 in the quiet tail",
              "zero at " + std::to_string(r.zeroAtMs) + " ms");
    }

    // --------------------------------------------------- 4) loud paths intact
    std::printf("\n== 4. loud sources still classify correctly ==\n");
    {
        Buf b = MakeBuf(2.0);
        FloorNoise(b, -60.0);
        for (int i = 0; i < 3; ++i)
            Burst(b, 0, static_cast<size_t>((0.3 + 0.25 * i) * kFs), 0.5, 200.0, 150.0);
        Crack(b, 5, static_cast<size_t>(1.6 * kFs), 0.9, 20.0, 0.0); // sharp crack, no thump
        Run r = RunPipeline(b, ClsCfg(), ClassifyConfig());
        Check(r.obs.step[0], "loud footsteps on FL -> footstep", ClassesOf(r.obs));
        Check(!r.obs.step[5], "crack on BR -> not a footstep", ClassesOf(r.obs));
    }

    // ------------------------------------------- 5) no runaway on a loud floor
    std::printf("\n== 5. loud ambience then a quiet step: the gate must recover ==\n");
    {
        Buf b = MakeBuf(2.9);
        // 1.0 s loud ambience (-22 dBFS), then quiet ambience (-70 dBFS) with
        // steps 28 dB above that floor.
        for (size_t i = 0; i < static_cast<size_t>(1.0 * kFs) * kCh; ++i)
            b.pcm[i] = Rand11() * static_cast<float>(DbToLin(-22.0));
        for (size_t i = static_cast<size_t>(1.0 * kFs) * kCh; i < b.pcm.size(); ++i)
            b.pcm[i] = Rand11() * static_cast<float>(DbToLin(-70.0));
        for (int i = 0; i < 3; ++i)
            Burst(b, 0, static_cast<size_t>((2.0 + 0.25 * i) * kFs), DbToLin(-42.0), 200.0, 150.0);
        Run r = RunPipeline(b, ClsCfg(), ClassifyConfig());
        Check(r.obs.step[0], "quiet footsteps after loud ambience -> footstep",
              ClassesOf(r.obs) + " lvl " + std::to_string(r.peakLevel) +
                  " gateBlocks " + std::to_string(r.gateBlocks));
        Check(!r.obs.step[5], "quiet ambience did not fire a false arrow", ClassesOf(r.obs));
    }

    // ------------------------------------------------- 6) downmix mode matrix
    std::printf("\n== 6. downmix modes (stereo / right-mono / left-mono) ==\n");
    {
        const size_t frames = 9600;
        auto synth = [&](int ch, double freq, double amp, std::vector<float>& in) {
            in.assign(frames * kCh, 0.0f);
            for (size_t f = 0; f < frames; ++f) {
                const double t = static_cast<double>(f) / kFs;
                in[f * kCh + ch] = static_cast<float>(amp * std::sin(2.0 * M_PI * freq * t));
            }
        };
        auto peaks = [&](const std::vector<float>& out, double& l, double& r) {
            l = 0;
            r = 0;
            for (size_t f = 0; f < frames; ++f) {
                l = std::max(l, std::fabs(static_cast<double>(out[f * 2])));
                r = std::max(r, std::fabs(static_cast<double>(out[f * 2 + 1])));
            }
        };
        std::vector<float> in, out(frames * 2);
        double l = 0, r = 0;

        sr::DownmixConfig sc;
        synth(0, 500.0, 0.2, in);
        sr::Downmix8To2(in.data(), out.data(), frames, sc);
        peaks(out, l, r);
        Check(l > 0.15 && r < 0.01, "stereo: FL -> left only");

        sr::DownmixConfig rc;
        rc.mode = sr::DownmixRightMono;
        synth(4, 500.0, 0.2, in); // BL: far from the front pair
        sr::Downmix8To2(in.data(), out.data(), frames, rc);
        peaks(out, l, r);
        Check(r > 0.1 && l == 0.0, "right-mono: BL -> right, left silent");

        sr::DownmixConfig lc;
        lc.mode = sr::DownmixLeftMono;
        sr::Downmix8To2(in.data(), out.data(), frames, lc);
        peaks(out, l, r);
        Check(l > 0.1 && r == 0.0, "left-mono: BL -> left, right silent");

        bool allCh = true;
        for (sr::DownmixMode m : { sr::DownmixRightMono, sr::DownmixLeftMono }) {
            for (int ch = 0; ch < kCh; ++ch) {
                sr::DownmixConfig c;
                c.mode = m;
                synth(ch, 300.0 + 100.0 * ch, 0.2, in);
                sr::Downmix8To2(in.data(), out.data(), frames, c);
                peaks(out, l, r);
                const double active = (m == sr::DownmixRightMono) ? r : l;
                const double quiet = (m == sr::DownmixRightMono) ? l : r;
                if (!(active > 0.1 && quiet == 0.0)) allCh = false;
            }
        }
        Check(allCh, "both mono modes carry all 8 channels to the active ear");

        sr::DownmixMode parsed = sr::DownmixStereo;
        Check(std::strcmp(sr::DownmixModeName(sr::DownmixLeftMono), "left-mono") == 0 &&
                  sr::DownmixModeFromName("left-mono", parsed) &&
                  parsed == sr::DownmixLeftMono &&
                  !sr::DownmixModeFromName("garbage", parsed) &&
                  parsed == sr::DownmixLeftMono,
              "mode names are stable, parse back, and reject unknown names");
    }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "ALL PROBES PASSED" : "PROBES FAILED",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
