// overlay.cpp - DirectComposition + Direct2D overlay implementation.
//
// Design: thin double ring (hollow center - the player sees through), tick
// marks at the channel angles, a slow rotating sweep wedge, and MOVING
// direction arrows: an energy-cluster estimator finds peaks in the circular
// channel arrangement, computes an energy-weighted centroid angle per peak
// (so a 70% FL / 30% C sound points between them), and tracks arrows across
// frames with shortest-path angular smoothing. Multiple separated peaks =
// multiple arrows. FX slider scales glow/trails/sweep; 0 = flat minimal.
//
// Why DComp and not UpdateLayeredWindowIndirect: full-screen per-pixel-alpha
// pushes through ULW cost ~8 MB CPU copies per frame; DComp is composed by
// DWM on the GPU. The window must be SHOWN in production (a hidden window is
// never composed); headless tests pass visible=false and Present DO_NOT_WAIT.
#include "overlay.h"

#include "../engine/log.h"          // sr::Log
#include "../engine/wasapi_util.h" // ComInit

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "dcomp")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "windowscodecs")

namespace sr {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kPi = 3.14159265358979f;

// Channel order: FL FR C LFE BL BR SL SR. Angle: 0 deg = up/front, clockwise
// positive; LFE has no direction (it brightens the ring instead).
constexpr float kAngleDeg[8] = { -30.f, 30.f, 0.f, -999.f, -135.f, 135.f, -90.f, 90.f };
constexpr wchar_t kLabel[8][4] = { L"FL", L"FR", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR" };

// ring order sorted by angle (circular adjacency): BL SL FL C FR SR BR
constexpr int kRingCh[7] = { 4, 6, 0, 2, 1, 7, 5 };
constexpr float kRingAng[7] = { -135.f, -90.f, -30.f, 0.f, 30.f, 90.f, 135.f };

// shortest-path angular difference a-b, in -180..180
float AngDist(float a, float b) {
    float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}

float NormAngle(float a) {
    a = std::fmod(a + 540.0f, 360.0f);
    return a - 180.0f;
}

// --- direction estimation: peak cluster centroid + cross-frame tracking ----

class ArrowTracker {
public:
    struct Arrow {
        float angle = 0;    // smoothed display angle, degrees
        float strength = 0; // smoothed energy
        float pulse = 0;    // onset ripple 0..1 (0 = none), ~150 ms
        float age = 0;      // seconds since spawn (scale-in pop)
        float trail0 = 0, trail1 = 0; // recent older angles (comet trail)
        bool matched = false;
    };

    // minLevel: display threshold; dt: seconds since last call.
    void Update(const float levels[8], float minLevel, float dt) {
        // 1) candidate peaks: level >= both ring neighbors, above threshold
        bool cand[7];
        for (int i = 0; i < 7; ++i) {
            float l = levels[kRingCh[i]];
            float lp = levels[kRingCh[(i + 6) % 7]];
            float ln = levels[kRingCh[(i + 1) % 7]];
            cand[i] = l > minLevel && l >= lp && l >= ln;
        }
        // 2) maximal circular runs of adjacent candidates -> one peak each,
        //    centroid = energy-weighted circular mean over run +/- 1 neighbor
        struct Peak { float angle, energy; };
        Peak peaks[4];
        int nPeaks = 0;
        int start = -1;
        for (int i = 0; i < 7; ++i)
            if (!cand[i] && cand[(i + 1) % 7]) { start = (i + 1) % 7; break; }
        if (start >= 0) {
            int i = start;
            do {
                if (!cand[i]) { i = (i + 1) % 7; continue; }
                int a = i, b = i;
                while (cand[(b + 1) % 7] && (b + 1) % 7 != start) b = (b + 1) % 7;
                if (a == b && cand[(b + 1) % 7]) break; // all 7: no direction
                double sx = 0, sy = 0;
                float emax = 0;
                for (int j = (a + 6) % 7;; j = (j + 1) % 7) {
                    float w = levels[kRingCh[j]];
                    float rad = kRingAng[j] * kPi / 180.0f;
                    sx += w * std::sin(rad);
                    sy += w * std::cos(rad);
                    if (w > emax) emax = w;
                    if (j == (b + 1) % 7) break;
                }
                if (nPeaks < 4 && (sx * sx + sy * sy) > 1e-6) {
                    peaks[nPeaks].angle = NormAngle(
                        static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPi);
                    peaks[nPeaks].energy = emax;
                    ++nPeaks;
                }
                i = (b + 1) % 7;
            } while (i != start && nPeaks < 4);
        }

        // 3) match peaks to existing arrows (< 60 deg), else spawn
        float smoothA = 1.0f - std::exp(-dt / 0.08f); // 80 ms glide
        for (auto& ar : arrows_) ar.matched = false;
        for (int p = 0; p < nPeaks; ++p) {
            int best = -1;
            float bestD = 60.0f;
            for (size_t k = 0; k < arrows_.size(); ++k) {
                if (arrows_[k].matched) continue;
                float d = std::fabs(AngDist(peaks[p].angle, arrows_[k].angle));
                if (d < bestD) { bestD = d; best = static_cast<int>(k); }
            }
            if (best >= 0) {
                Arrow& ar = arrows_[best];
                ar.matched = true;
                ar.trail1 = ar.trail0;
                ar.trail0 = ar.angle;
                ar.angle = NormAngle(ar.angle + AngDist(peaks[p].angle, ar.angle) * smoothA);
                ar.strength += (peaks[p].energy - ar.strength) * smoothA;
                if (ar.pulse > 0.0f) {
                    ar.pulse += dt / 0.15f; // ~150 ms onset ripple
                    if (ar.pulse >= 1.0f) ar.pulse = 0.0f;
                }
            } else {
                Arrow ar;
                ar.angle = ar.trail0 = ar.trail1 = peaks[p].angle;
                ar.strength = peaks[p].energy;
                ar.pulse = 0.001f; // onset ripple
                ar.matched = true;
                arrows_.push_back(ar);
            }
        }
        // unmatched arrows fade out; all arrows age (scale-in pop)
        for (size_t k = 0; k < arrows_.size();) {
            arrows_[k].age += dt;
            if (!arrows_[k].matched) {
                arrows_[k].strength *= std::exp(-dt / 0.15f);
                if (arrows_[k].strength < 0.02f) {
                    arrows_.erase(arrows_.begin() + k);
                    continue;
                }
            }
            ++k;
        }
    }

    const std::vector<Arrow>& Arrows() const { return arrows_; }

private:
    std::vector<Arrow> arrows_;
};

// Green -> yellow -> red, smoothly interpolated (no hard jumps at thresholds).
D2D1_COLOR_F LevelColor(float lvl, const OverlayConfig& cfg, float alphaScale = 1.0f) {
    float a = (lvl <= 0.0f) ? 0.0f : (lvl > 1.0f ? 1.0f : lvl);
    a *= alphaScale;
    const float G[3] = { 0.20f, 1.00f, 0.30f };
    const float Y[3] = { 1.00f, 0.85f, 0.10f };
    const float R[3] = { 1.00f, 0.15f, 0.10f };
    float r, g, b;
    if (lvl <= cfg.lowThreshold) {
        r = G[0]; g = G[1]; b = G[2];
    } else if (lvl <= cfg.highThreshold) {
        float t = (lvl - cfg.lowThreshold) /
                  (cfg.highThreshold - cfg.lowThreshold + 1e-6f);
        r = G[0] + (Y[0] - G[0]) * t;
        g = G[1] + (Y[1] - G[1]) * t;
        b = G[2] + (Y[2] - G[2]) * t;
    } else {
        float t = (lvl - cfg.highThreshold) / (1.0f - cfg.highThreshold + 1e-6f);
        if (t > 1.0f) t = 1.0f;
        r = Y[0] + (R[0] - Y[0]) * t;
        g = Y[1] + (R[1] - Y[1]) * t;
        b = Y[2] + (R[2] - Y[2]) * t;
    }
    return D2D1::ColorF(r, g, b, a);
}

// --- cached scene resources (rebuilt when the live config version moves) ---

struct SceneResources {
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1PathGeometry> arrow;  // chevron pointing up, on the ring radius
    ComPtr<IDWriteTextLayout> fsLayout;     // "FS" footstep marker
    ComPtr<IDWriteTextLayout> gsLayout;     // "GS" gunshot marker
    ComPtr<IDWriteTextLayout> cornerLayout; // "实验性 Experimental"
};

// Tapered chevron in radar-local coords pointing up (0 deg): tip outside the
// ring, base on the ring, inner notch. Rotated per arrow at draw time.
ComPtr<ID2D1PathGeometry> MakeArrow(ID2D1Factory* factory, float cx, float cy, float r) {
    const float halfW = 10.0f, tipLen = 17.0f, notch = 6.0f;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return nullptr;
    sink->BeginFigure(D2D1::Point2F(cx, cy - (r + tipLen)), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx + halfW, cy - r));
    sink->AddLine(D2D1::Point2F(cx, cy - (r + notch)));
    sink->AddLine(D2D1::Point2F(cx - halfW, cy - r));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

HRESULT CreateSceneResources(ID2D1Factory* d2dFactory, IDWriteFactory* dwFactory,
                             ID2D1RenderTarget* rt, const OverlayConfig& cfg,
                             int w, int h, SceneResources& res) {
    HRESULT hr = rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &res.brush);
    if (FAILED(hr)) return hr;
    hr = rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.5f), &res.textBrush);
    if (FAILED(hr)) return hr;

    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float r = static_cast<float>(cfg.radius);

    res.arrow = MakeArrow(d2dFactory, cx, cy, r);
    if (!res.arrow) return E_FAIL;

    ComPtr<IDWriteTextFormat> fmt;
    hr = dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     10.0f, L"", &fmt);
    if (FAILED(hr)) return hr;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    hr = dwFactory->CreateTextLayout(L"FS", 2, fmt.Get(), 48.0f, 24.0f, &res.fsLayout);
    if (FAILED(hr)) return hr;
    hr = dwFactory->CreateTextLayout(L"GS", 2, fmt.Get(), 48.0f, 24.0f, &res.gsLayout);
    if (FAILED(hr)) return hr;
    ComPtr<IDWriteTextFormat> cornerFmt;
    hr = dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     12.0f, L"zh-cn", &cornerFmt);
    if (FAILED(hr)) return hr;
    const wchar_t* corner = L"实验性 Experimental";
    hr = dwFactory->CreateTextLayout(corner, static_cast<UINT32>(wcslen(corner)),
                                     cornerFmt.Get(), 260.0f, 24.0f, &res.cornerLayout);
    return hr;
}

// Edge band as a 5-slice sideways-fading strip with a soft glow pass.
void DrawBand(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
              float lvl, const D2D1_RECT_F& span, int edge /*0=top 1=right 2=bottom 3=left*/,
              float fx) {
    if (lvl <= 0.02f) return; // zero level = invisible
    const float profile[5] = { 0.20f, 0.55f, 1.0f, 0.55f, 0.20f };
    float t = 2.0f + 14.0f * (lvl > 1.0f ? 1.0f : lvl);
    bool horiz = (edge == 0 || edge == 2);
    float lo = horiz ? span.left : span.top;
    float hi = horiz ? span.right : span.bottom;
    float len = hi - lo;
    for (int pass = 0; pass < 2; ++pass) {
        float tPass = (pass == 0) ? t + 8.0f * fx : t; // pass 0 = glow
        float aMul = (pass == 0) ? 0.25f * fx : 0.8f;
        if (aMul <= 0.0f) continue;
        for (int i = 0; i < 5; ++i) {
            float a0 = lo + len * i / 5.0f, a1 = lo + len * (i + 1) / 5.0f;
            D2D1_RECT_F r;
            switch (edge) {
                case 0: r = D2D1::RectF(a0, span.top, a1, span.top + tPass); break;
                case 1: r = D2D1::RectF(span.right - tPass, a0, span.right, a1); break;
                case 2: r = D2D1::RectF(a0, span.bottom - tPass, a1, span.bottom); break;
                default: r = D2D1::RectF(span.left, a0, span.left + tPass, a1); break;
            }
            res.brush->SetColor(LevelColor(lvl, cfg, profile[i] * aMul));
            rt->FillRectangle(&r, res.brush.Get());
        }
    }
}

// sweepDeg < 0 = no sweep (idle).
void DrawScene(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
               const float levels[8], const std::vector<ArrowTracker::Arrow>& arrows,
               const uint8_t* classes, bool classifyOn, int w, int h) {
    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float r = static_cast<float>(cfg.radius);
    float fx = cfg.fxPct / 100.0f;

    // tracked direction arrows ONLY (no ring, no labels - center stays clear)
    for (const auto& a : arrows) {
        if (a.strength <= 0.02f) continue;
        float thickness = 1.5f + 2.5f * a.strength;
        D2D1_COLOR_F col = LevelColor(a.strength, cfg);
        // scale-in pop over ~150 ms after spawn
        float pop = (a.age < 0.15f) ? 0.6f + 0.4f * (a.age / 0.15f) : 1.0f;
        auto arrowXform = [&](float deg, float s) {
            return D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, cy - r)) *
                   D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(cx, cy));
        };

        // comet trail: two fading copies at recent older angles
        if (fx > 0.0f) {
            const float trails[2] = { a.trail1, a.trail0 };
            const float trailA[2] = { 0.12f, 0.25f };
            for (int ti = 0; ti < 2; ++ti) {
                if (trails[ti] == a.angle) continue;
                rt->SetTransform(arrowXform(trails[ti], pop * 0.9f));
                res.brush->SetColor(LevelColor(a.strength, cfg, trailA[ti] * fx));
                rt->FillGeometry(res.arrow.Get(), res.brush.Get());
            }
        }

        rt->SetTransform(arrowXform(a.angle, pop));
        if (fx > 0.0f) { // soft glow halo
            res.brush->SetColor(LevelColor(a.strength, cfg, 0.12f * fx));
            rt->DrawGeometry(res.arrow.Get(), res.brush.Get(), thickness + 6.0f * fx);
            res.brush->SetColor(LevelColor(a.strength, cfg, 0.28f * fx));
            rt->DrawGeometry(res.arrow.Get(), res.brush.Get(), thickness + 3.0f * fx);
        }
        res.brush->SetColor(col); // bright core
        rt->FillGeometry(res.arrow.Get(), res.brush.Get());
        rt->DrawGeometry(res.arrow.Get(), res.brush.Get(), thickness);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());

        // onset ripple flash at the arrow's ring position
        if (a.pulse > 0.0f && fx > 0.0f) {
            float rad = a.angle * kPi / 180.0f;
            D2D1_POINT_2F at = D2D1::Point2F(cx + r * std::sin(rad),
                                             cy - r * std::cos(rad));
            float rr = 4.0f + 14.0f * a.pulse;
            res.brush->SetColor(LevelColor(a.strength, cfg, (1.0f - a.pulse) * 0.5f * fx));
            rt->DrawEllipse(D2D1::Ellipse(at, rr, rr), res.brush.Get(), 1.5f);
        }
    }

    // experimental classification markers: small text just outside the channel
    // angle position (no clutter)
    if (classifyOn && classes) {
        for (int c = 0; c < 8; ++c) {
            uint8_t cls = classes[c];
            if (cls == SoundNone || c == 3) continue; // LFE has no direction
            float rad = kAngleDeg[c] * kPi / 180.0f;
            D2D1_POINT_2F mp = D2D1::Point2F(cx + (r + 34.0f) * std::sin(rad) - 24.0f,
                                             cy - (r + 34.0f) * std::cos(rad) - 12.0f);
            if (cls == SoundFootstep) {
                res.textBrush->SetColor(D2D1::ColorF(1.0f, 0.6f, 0.1f, 0.9f));
                rt->DrawTextLayout(mp, res.fsLayout.Get(), res.textBrush.Get());
            } else if (cls == SoundGunshot) {
                res.textBrush->SetColor(D2D1::ColorF(1.0f, 0.25f, 0.15f, 0.95f));
                rt->DrawTextLayout(mp, res.gsLayout.Get(), res.textBrush.Get());
            }
        }
        res.textBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        rt->DrawTextLayout(D2D1::Point2F(8.0f, static_cast<float>(h) - 26.0f),
                           res.cornerLayout.Get(), res.textBrush.Get());
    }

    // edge bands
    float W = static_cast<float>(w), H = static_cast<float>(h);
    DrawBand(rt, res, cfg, levels[0], D2D1::RectF(0, 0, W / 3, 0), 0, fx);          // top FL
    DrawBand(rt, res, cfg, levels[2], D2D1::RectF(W / 3, 0, 2 * W / 3, 0), 0, fx);  // top C
    DrawBand(rt, res, cfg, levels[1], D2D1::RectF(2 * W / 3, 0, W, 0), 0, fx);      // top FR
    DrawBand(rt, res, cfg, levels[1], D2D1::RectF(W, 0, W, H / 3), 1, fx);          // right FR
    DrawBand(rt, res, cfg, levels[7], D2D1::RectF(W, H / 3, W, 2 * H / 3), 1, fx);  // right SR
    DrawBand(rt, res, cfg, levels[5], D2D1::RectF(W, 2 * H / 3, W, H), 1, fx);      // right BR
    DrawBand(rt, res, cfg, levels[4], D2D1::RectF(0, H, W / 2, H), 2, fx);          // bottom BL
    DrawBand(rt, res, cfg, levels[5], D2D1::RectF(W / 2, H, W, H), 2, fx);          // bottom BR
    DrawBand(rt, res, cfg, levels[0], D2D1::RectF(0, 0, 0, H / 3), 3, fx);          // left FL
    DrawBand(rt, res, cfg, levels[6], D2D1::RectF(0, H / 3, 0, 2 * H / 3), 3, fx);  // left SL
    DrawBand(rt, res, cfg, levels[4], D2D1::RectF(0, 2 * H / 3, 0, H), 3, fx);      // left BL
}

const wchar_t* kWndClass = L"SoundRadarOverlay";

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; // never intercept clicks, even if styles fail
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// CPU accounting helper: GetThreadTimes (100 ns ticks) vs GetTickCount64 wall.
struct CpuProbe {
    HANDLE self = GetCurrentThread();
    uint64_t cpu0 = 0, wall0 = 0;
    static uint64_t CpuNow(HANDLE t) {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(t, &c, &e, &k, &u)) return 0;
        return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
               (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    }
    void Begin() {
        cpu0 = CpuNow(self);
        wall0 = GetTickCount64();
    }
    void End(std::atomic<uint64_t>& cpuAcc, std::atomic<uint64_t>& wallAcc) {
        uint64_t cpu1 = CpuNow(self);
        uint64_t wall1 = GetTickCount64();
        cpuAcc.fetch_add(cpu1 - cpu0, std::memory_order_relaxed);
        wallAcc.fetch_add((wall1 - wall0) * 10000, std::memory_order_relaxed);
    }
};

} // namespace

Overlay::~Overlay() { Stop(); }

bool Overlay::Start(const OverlayConfig& cfg, SharedMeters* meters, HANDLE quitEvent,
                    bool visible) {
    if (running_.load()) return true;
    cfg_ = cfg;
    {
        // publish as the live config so Apply works even before the first frame
        std::lock_guard<std::mutex> lk(g_overlay.mu);
        g_overlay.cfg = cfg;
        ++g_overlay.version;
    }
    meters_ = meters;
    quitEvent_ = quitEvent;
    if (!stopEvent_) stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(stopEvent_);
    ResetStats();
    frames_.store(0);
    appliedVersion_.store(0);
    hwnd_.store(nullptr);
    thread_ = std::thread(&Overlay::ThreadMain, this, visible);
    return true;
}

void Overlay::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    hwnd_.store(nullptr);
}

Overlay::Stats Overlay::GetStats() const {
    Stats s;
    uint64_t ac = activeCpu_.load(), aw = activeWall_.load();
    uint64_t ic = idleCpu_.load(), iw = idleWall_.load();
    s.activeCpuPct = aw ? 100.0 * static_cast<double>(ac) / aw : 0.0;
    s.idleCpuPct = iw ? 100.0 * static_cast<double>(ic) / iw : 0.0;
    s.frames = frames_.load();
    return s;
}

void Overlay::ResetStats() {
    activeCpu_ = activeWall_ = idleCpu_ = idleWall_ = 0;
}

void Overlay::ThreadMain(bool visible) {
    running_.store(true);
    ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"overlay: COM init failed\n");
        running_.store(false);
        return;
    }

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fwprintf(stderr, L"overlay: RegisterClassEx failed\n");
        running_.store(false);
        return;
    }

    const DWORD exStyle = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST |
                          WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    HWND hwnd = CreateWindowExW(exStyle, kWndClass, L"SoundRadar Overlay", WS_POPUP,
                                0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fwprintf(stderr, L"overlay: CreateWindowEx failed (%lu)\n", GetLastError());
        running_.store(false);
        return;
    }
    // visible=false is for headless tests only: a hidden window is never
    // composed by DWM, so nothing would reach the screen in production.
    if (visible) ShowWindow(hwnd, SW_SHOWNA); // shown, but never takes focus
    hwnd_.store(hwnd);
    sr::Log("overlay: window created (visible=%d)", visible ? 1 : 0);

    // --- D3D11 -> DXGI -> D2D device context -------------------------------
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    SceneResources res;
    bool initOk = false;

    if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                               nullptr,
                               reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (SUCCEEDED(hr))
        hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory2;
        dxgiDevice->GetAdapter(&adapter);
        if (adapter) adapter->GetParent(IID_PPV_ARGS(&factory2));
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = factory2 ? factory2->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr,
                                                                &swapchain)
                      : E_FAIL;
    }
    if (SUCCEEDED(hr))
        hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice));
    if (SUCCEEDED(hr)) hr = dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget);
    if (SUCCEEDED(hr)) hr = dcompDevice->CreateVisual(&dcompVisual);
    if (SUCCEEDED(hr)) hr = dcompVisual->SetContent(swapchain.Get());
    if (SUCCEEDED(hr)) hr = dcompTarget->SetRoot(dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = dcompDevice->Commit();
    if (SUCCEEDED(hr))
        hr = CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), dc.Get(), cfg_, w, h, res);
    initOk = SUCCEEDED(hr);
    sr::Log("overlay: device init %s (hr=0x%08lx)", initOk ? "ok" : "FAILED",
            (unsigned long)hr);
    if (!initOk)
        fwprintf(stderr, L"overlay: device init failed (hr=0x%08lx)\n",
                 static_cast<unsigned long>(hr));
    uint32_t seenCfgVersion = 0;
    {
        std::lock_guard<std::mutex> lk(g_overlay.mu);
        seenCfgVersion = g_overlay.version;
    }
    appliedVersion_.store(seenCfgVersion);

    ArrowTracker tracker;
    uint64_t lastTick = GetTickCount64();
    int presentLogCount = 0;

    // --- render loop: ~60 fps while audio active, ~4 fps polling when idle --
    HANDLE waits[2] = { quitEvent_, stopEvent_ };
    while (initOk) {
        // drain any window messages (rare: we never take input)
        MSG msg;
        while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        // hot-apply overlay config (GUI Apply bumps g_overlay.version)
        {
            std::lock_guard<std::mutex> lk(g_overlay.mu);
            if (g_overlay.version != seenCfgVersion) {
                cfg_ = g_overlay.cfg;
                seenCfgVersion = g_overlay.version;
                CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), dc.Get(), cfg_,
                                     w, h, res);
                appliedVersion_.store(seenCfgVersion);
            }
        }

        AnalysisFrame frame;
        uint8_t classes[8] = {};
        {
            std::lock_guard<std::mutex> lk(meters_->mu);
            frame = meters_->frame;
            std::memcpy(classes, meters_->classes, sizeof(classes));
        }
        float maxLvl = 0.0f;
        for (int c = 0; c < 8; ++c) if (frame.level[c] > maxLvl) maxLvl = frame.level[c];
        bool active = frame.active || maxLvl > 0.02f;

        // direction estimation
        uint64_t nowTick = GetTickCount64();
        float dt = static_cast<float>(nowTick - lastTick) / 1000.0f;
        lastTick = nowTick;
        if (active) {
            tracker.Update(frame.level, 0.05f, dt);
        } else {
            float zeros[8] = {};
            tracker.Update(zeros, 0.05f, dt); // lets arrows fade out
        }
        {
            std::lock_guard<std::mutex> lk(debugMu_);
            debugAngles_.clear();
            for (const auto& a : tracker.Arrows()) debugAngles_.push_back(a.angle);
        }

        // measure the whole iteration (sleep included) for a true loop CPU%
        CpuProbe probe;
        probe.Begin();
        DWORD timeout = active ? 16 : 250; // 60 fps / 4 fps
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (wr != WAIT_TIMEOUT) break; // quit or stop

        if (active) {
            ComPtr<IDXGISurface> surface;
            HRESULT fhr = swapchain->GetBuffer(0, IID_PPV_ARGS(&surface));
            ComPtr<ID2D1Bitmap1> target;
            if (SUCCEEDED(fhr)) {
                D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED));
                fhr = dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target);
            }
            if (SUCCEEDED(fhr)) {
                dc->SetTarget(target.Get());
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(0, 0, 0, 0)); // fully transparent base
                DrawScene(dc.Get(), res, cfg_, frame.level, tracker.Arrows(),
                          classes, g_classifyEnabled.load(), w, h);
                fhr = dc->EndDraw();
                dc->SetTarget(nullptr);
            }
            // Hidden window (tests): DWM never consumes frames, so a blocking
            // Present would deadlock once the flip queue fills -> DO_NOT_WAIT.
            if (SUCCEEDED(fhr))
                fhr = swapchain->Present(0, visible ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
            if (fhr == DXGI_ERROR_WAS_STILL_DRAWING) fhr = S_OK; // dropped frame, fine
            if (SUCCEEDED(fhr)) fhr = dcompDevice->Commit();
            if (presentLogCount < 10)
                sr::Log("overlay: present[%d] hr=0x%08lx", presentLogCount++,
                        (unsigned long)fhr);
            if (fhr == D2DERR_RECREATE_TARGET || fhr == DXGI_ERROR_DEVICE_REMOVED ||
                fhr == DXGI_ERROR_DEVICE_RESET) {
                fwprintf(stderr, L"overlay: device lost, stopping overlay\n");
                break; // device-lost recovery left for a later milestone
            }
            frames_.fetch_add(1, std::memory_order_relaxed);
            probe.End(activeCpu_, activeWall_);
        } else {
            probe.End(idleCpu_, idleWall_);
        }
    }

    // full teardown: window + all D2D/DComp resources die with the thread
    res = SceneResources{};
    dc.Reset();
    d2dDevice.Reset();
    dcompVisual.Reset();
    dcompTarget.Reset();
    dcompDevice.Reset();
    swapchain.Reset();
    DestroyWindow(hwnd);
    hwnd_.store(nullptr);
    running_.store(false);
}

// --- WIC screenshot path -----------------------------------------------------

bool RenderSceneToFile(const std::wstring& path, int width, int height,
                       const float levels[8], const uint8_t* classes,
                       const OverlayConfig& cfg) {
    ComInit com;
    if (!com.Ok()) return false;

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&wic));
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                               nullptr,
                               reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));

    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(hr))
        hr = wic->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                               GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    ComPtr<ID2D1RenderTarget> rt;
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = d2dFactory->CreateWicBitmapRenderTarget(bitmap.Get(), &props, &rt);
    }
    SceneResources res;
    if (SUCCEEDED(hr))
        hr = CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), rt.Get(), cfg,
                                  width, height, res);
    if (SUCCEEDED(hr)) {
        // one-shot tracker run (long dt -> snaps to centroids, ripple mid-way)
        ArrowTracker tracker;
        tracker.Update(levels, 0.05f, 0.5f);
        tracker.Update(levels, 0.05f, 0.1f);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.06f, 0.06f, 0.09f, 1.0f)); // opaque dark backdrop
        DrawScene(rt.Get(), res, cfg, levels, tracker.Arrows(), classes,
                  classes != nullptr, width, height);
        hr = rt->EndDraw();
    }
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatBmp, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pf);
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(bitmap.Get(), pf, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = frame->WriteSource(conv.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return SUCCEEDED(hr);
}

} // namespace sr
