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

#include "arrow_tracker.h" // ArrowTracker, angle helpers (host-testable)
#include "mouse_turn.h"    // RotateLevels, MouseDegPerCount

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
constexpr wchar_t kLabel[8][4] = { L"FL", L"FR", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR" };

using ArrowTracker = sr::ArrowTracker;

// Neon sonar palette: cool cyan->teal below the low threshold, teal->amber up
// to the high threshold, amber->red-magenta above. Threshold config semantics
// unchanged. Alpha floor ~0.60 while active (readability).
D2D1_COLOR_F NeonColor(float lvl, const OverlayConfig& cfg, float alphaScale = 1.0f) {
    static const float C0[3] = { 0.20f, 0.88f, 1.00f }; // cyan (far/weak)
    static const float C1[3] = { 0.00f, 0.82f, 0.75f }; // teal
    static const float C2[3] = { 1.00f, 0.70f, 0.13f }; // amber
    static const float C3[3] = { 1.00f, 0.18f, 0.42f }; // red-magenta (near/loud)
    float r, g, b;
    if (lvl <= cfg.lowThreshold) {
        float t = cfg.lowThreshold > 1e-6f ? lvl / cfg.lowThreshold : 0.0f;
        r = C0[0] + (C1[0] - C0[0]) * t; g = C0[1] + (C1[1] - C0[1]) * t;
        b = C0[2] + (C1[2] - C0[2]) * t;
    } else if (lvl <= cfg.highThreshold) {
        float t = (lvl - cfg.lowThreshold) / (cfg.highThreshold - cfg.lowThreshold + 1e-6f);
        r = C1[0] + (C2[0] - C1[0]) * t; g = C1[1] + (C2[1] - C1[1]) * t;
        b = C1[2] + (C2[2] - C1[2]) * t;
    } else {
        float t = (lvl - cfg.highThreshold) / (1.0f - cfg.highThreshold + 1e-6f);
        if (t > 1.0f) t = 1.0f;
        r = C2[0] + (C3[0] - C2[0]) * t; g = C2[1] + (C3[1] - C2[1]) * t;
        b = C2[2] + (C3[2] - C2[2]) * t;
    }
    float cl = (lvl > 1.0f) ? 1.0f : lvl;
    float a = (lvl <= 0.0f) ? 0.0f : (0.60f + 0.40f * cl);
    return D2D1::ColorF(r, g, b, a * alphaScale);
}

// --- cached scene resources (rebuilt when the live config version moves) ---

struct SceneResources {
    ComPtr<ID2D1Factory> factory;           // arcs are built per frame (span varies)
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1StrokeStyle> roundCaps;     // round-capped arc strokes
    ComPtr<ID2D1PathGeometry> star;         // gunshot starburst, local origin
    ComPtr<IDWriteTextLayout> cornerLayout; // "实验性 Experimental"
};

// Ring arrowhead: chevron with a notched tail. Base sits ON the invisible
// ring at (cx, cy - ringR), tip points outward (up). Built per frame because
// the size scales with loudness. Rotated around the center per cluster.
ComPtr<ID2D1PathGeometry> MakeArrowAt(ID2D1Factory* factory, float cx, float cy,
                                      float ringR, float len, float halfW) {
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return nullptr;
    float yBase = cy - ringR;
    sink->BeginFigure(D2D1::Point2F(cx, yBase - len), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx + halfW, yBase));
    sink->AddLine(D2D1::Point2F(cx, yBase - len * 0.35f)); // tail notch
    sink->AddLine(D2D1::Point2F(cx - halfW, yBase));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

// 8-point starburst polygon (gunshot icon), local coords centered at origin.
ComPtr<ID2D1PathGeometry> MakeStar(ID2D1Factory* factory) {
    const float rOut = 9.5f, rIn = 3.8f;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return nullptr;
    for (int i = 0; i < 16; ++i) {
        float a = i * kPi / 8.0f;
        float r = (i % 2 == 0) ? rOut : rIn;
        D2D1_POINT_2F p = D2D1::Point2F(r * std::sin(a), -r * std::cos(a));
        if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
        else sink->AddLine(p);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

HRESULT CreateSceneResources(ID2D1Factory* d2dFactory, IDWriteFactory* dwFactory,
                             ID2D1RenderTarget* rt, SceneResources& res) {
    res.factory = d2dFactory; // arrowheads are built per frame (size animates)
    HRESULT hr = rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &res.brush);
    if (FAILED(hr)) return hr;
    hr = rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.5f), &res.textBrush);
    if (FAILED(hr)) return hr;

    D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};
    strokeProps.startCap = D2D1_CAP_STYLE_ROUND;
    strokeProps.endCap = D2D1_CAP_STYLE_ROUND;
    hr = d2dFactory->CreateStrokeStyle(&strokeProps, nullptr, 0, &res.roundCaps);
    if (FAILED(hr)) return hr;
    res.star = MakeStar(d2dFactory);
    if (!res.star) return E_FAIL;

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

// Directional line on the screen border: project the tracked angle onto the
// border rect (inset 10 px), draw a wavy thin round-capped line there with a
// faint wide under-glow. Quiet/far = long and green; loud/near = short and
// red, with a smooth morph between. Springy overshoot on onset plus a
// travelling sine wobble keep it "bouncy". Slides along the edge as the
// angle moves.
void DrawCapsule(ID2D1RenderTarget* rt, const SceneResources& res,
                 const OverlayConfig& cfg, float angleDeg, float lvl, float pulse,
                 int w, int h, float fx, float timeSec) {
    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float rad = angleDeg * kPi / 180.0f;
    float dx = std::sin(rad), dy = -std::cos(rad); // y down

    // ray -> border rect intersection
    const float inset = 10.0f;
    float left = inset, top = inset, right = w - inset, bottom = h - inset;
    float tBest = 1e9f;
    int edge = -1;
    if (dx > 1e-4f) { float t = (right - cx) / dx; if (t < tBest) { tBest = t; edge = 1; } }
    if (dx < -1e-4f) { float t = (left - cx) / dx; if (t < tBest) { tBest = t; edge = 3; } }
    if (dy > 1e-4f) { float t = (bottom - cy) / dy; if (t < tBest) { tBest = t; edge = 2; } }
    if (dy < -1e-4f) { float t = (top - cy) / dy; if (t < tBest) { tBest = t; edge = 0; } }
    if (edge < 0) return;
    float hx = cx + tBest * dx, hy = cy + tBest * dy;
    bool horiz = (edge == 0 || edge == 2);
    float tx = horiz ? 1.0f : 0.0f, ty = horiz ? 0.0f : 1.0f; // tangent along border
    // inward normal (into the screen): top +y, right -x, bottom -y, left +x
    float inx = (edge == 3) ? 1.0f : (edge == 1) ? -1.0f : 0.0f;
    float iny = (edge == 0) ? 1.0f : (edge == 2) ? -1.0f : 0.0f;

    float widMul = cfg.edgeWidthPct / 100.0f; // scales the main line width
    float lenMul = cfg.edgeLenPct / 100.0f;

    // loudness morph: quiet = long, loud = short
    float halfLen = (170.0f - 90.0f * lvl) * lenMul; // 170 -> 80 px
    float halfOff = 3.0f + 11.0f * lvl;              // inward offset from border

    // jelly bounce: springy overshoot at onset + gentle breathing wobble
    float bounce = 1.0f;
    if (pulse > 0.0f && pulse < 1.0f)
        bounce += 0.50f * std::sin(pulse * kPi)
                + 0.22f * std::sin(pulse * 2.0f * kPi) * (1.0f - pulse);
    bounce += 0.06f * std::sin(timeSec * 5.5f) * fx;
    halfOff *= bounce;
    halfLen *= 1.0f - 0.15f * (bounce - 1.0f); // bouncier -> a bit shorter

    const int K = 26; // line samples per edge
    float waveAmp = (1.5f + 3.5f * lvl) * fx;
    float phase = timeSec * 6.0f;

    // wavy center line riding `halfOff` inward from the border
    ComPtr<ID2D1PathGeometry> line;
    if (FAILED(res.factory->CreatePathGeometry(&line))) return;
    {
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(line->Open(&sink))) return;
        for (int i = 0; i <= K; ++i) {
            float u = static_cast<float>(i) / K;   // 0..1 along the line
            float along = u * 2.0f - 1.0f;         // -1..1
            float env = std::sin(u * kPi);         // wave fades at the tips
            float wave = waveAmp * std::sin(phase + along * 5.0f) * env;
            float px = hx + tx * along * halfLen + inx * (halfOff + wave);
            float py = hy + ty * along * halfLen + iny * (halfOff + wave);
            if (i == 0)
                sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW);
            else
                sink->AddLine(D2D1::Point2F(px, py));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
    }

    D2D1_COLOR_F col = NeonColor(lvl, cfg);

    // under-glow: one wide, very faint stroke (skipped when fx = 0)
    if (fx > 0.0f) {
        res.brush->SetColor(NeonColor(lvl, cfg, (0.10f + 0.08f * lvl) * fx));
        rt->DrawGeometry(line.Get(), res.brush.Get(), 8.0f + 10.0f * lvl,
                         res.roundCaps.Get());
    }

    // main line: thin round-capped stroke, dim tips -> saturated center
    {
        D2D1_COLOR_F dim = col, bright = col;
        dim.r *= 0.45f; dim.g *= 0.45f; dim.b *= 0.45f;
        bright.r += (1.0f - bright.r) * 0.30f;
        bright.g += (1.0f - bright.g) * 0.30f;
        bright.b += (1.0f - bright.b) * 0.30f;
        D2D1_GRADIENT_STOP stops[3];
        stops[0].position = 0.0f; stops[0].color = dim;
        stops[1].position = 0.5f; stops[1].color = bright;
        stops[2].position = 1.0f; stops[2].color = dim;
        ComPtr<ID2D1GradientStopCollection> gsc;
        rt->CreateGradientStopCollection(stops, 3, &gsc);
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (gsc)
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(hx - tx * halfLen, hy - ty * halfLen),
                    D2D1::Point2F(hx + tx * halfLen, hy + ty * halfLen)),
                gsc.Get(), &grad);
        float lineW = (2.0f + 4.0f * lvl) * widMul;
        if (grad)
            rt->DrawGeometry(line.Get(), grad.Get(), lineW, res.roundCaps.Get());
        else {
            res.brush->SetColor(col);
            rt->DrawGeometry(line.Get(), res.brush.Get(), lineW,
                             res.roundCaps.Get());
        }
    }
}

// Ring-arrow scene: an INVISIBLE circle around the crosshair; each tracked
// sound cluster gets a small chevron arrow sitting on that circle, tip
// pointing outward at the precise (continuous) angle. Multiple simultaneous
// directions = multiple independent arrows. Glow, hot core, motion trail,
// onset ripple, and vector type icons (footstep pair / gunshot spark).
void DrawScene(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
               const std::vector<ArrowTracker::Arrow>& arrows, float timeSec,
               float duckAmt, const uint8_t* classes, bool classifyOn, int w, int h) {
    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float r = static_cast<float>(cfg.radius);
    float fx = cfg.fxPct / 100.0f;

    // duck: soften frontal arrows (own gunfire/steps) while keys are held.
    // Full cut inside +-duckConeDeg, linear ramp over the next 20 deg.
    auto duckLevel = [&](float angle, float strength) {
        if (duckAmt <= 0.0f || !cfg.duckEnabled) return strength;
        float ad = std::fabs(ArrowAngDist(angle, 0.0f));
        float factor = 1.0f;
        if (ad <= cfg.duckConeDeg) factor = 1.0f - duckAmt;
        else if (ad < cfg.duckConeDeg + 20.0f)
            factor = 1.0f - duckAmt * (1.0f - (ad - cfg.duckConeDeg) / 20.0f);
        return strength * factor;
    };

    for (const auto& a : arrows) {
        float lvl = duckLevel(a.angle, a.strength);
        if (lvl <= 0.02f) continue;
        int bestCh = ArrowNearestChannel(a.angle);
        if (cfg.hideImpact && classifyOn && classes &&
            classes[bestCh] == SoundImpact)
            continue;
        // scale-in pop over ~150 ms after spawn
        float pop = (a.age < 0.15f) ? 0.6f + 0.4f * (a.age / 0.15f) : 1.0f;
        float len = (18.0f + 16.0f * lvl) * pop;   // 18..34 px, loud = longer
        float halfW = (9.0f + 5.0f * lvl) * pop;   // 9..14 px half width
        D2D1_COLOR_F col = NeonColor(lvl, cfg);

        auto drawArrow = [&](float deg, float scale, float alphaMul) {
            ComPtr<ID2D1PathGeometry> geo =
                MakeArrowAt(res.factory.Get(), cx, cy, r, len * scale, halfW * scale);
            if (!geo) return;
            rt->SetTransform(D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(cx, cy)));
            res.brush->SetColor(NeonColor(lvl, cfg, alphaMul));
            rt->FillGeometry(geo.Get(), res.brush.Get());
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        };

        // motion trail: two fading arrows at recent older angles
        if (fx > 0.0f) {
            const float trails[2] = { a.trail1, a.trail0 };
            const float trailA[2] = { 0.10f, 0.22f };
            for (int ti = 0; ti < 2; ++ti) {
                if (trails[ti] == a.angle) continue;
                drawArrow(trails[ti], 0.88f, trailA[ti] * fx);
            }
        }
        // soft glow: two enlarged dim copies behind the main arrow
        if (fx > 0.0f) {
            drawArrow(a.angle, 1.5f, 0.16f * fx);
            drawArrow(a.angle, 1.22f, 0.34f * fx);
        }
        // main arrow
        drawArrow(a.angle, 1.0f, 1.0f);
        // hot core: small bright chevron inside the main one
        {
            ComPtr<ID2D1PathGeometry> core =
                MakeArrowAt(res.factory.Get(), cx, cy, r - len * 0.06f,
                            len * 0.42f, halfW * 0.42f);
            if (core) {
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(a.angle, D2D1::Point2F(cx, cy)));
                res.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.125f + 0.175f * lvl));
                rt->FillGeometry(core.Get(), res.brush.Get());
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }

        // onset ripple flash at the arrow base
        if (a.pulse > 0.0f && fx > 0.0f) {
            float rad = a.angle * kPi / 180.0f;
            D2D1_POINT_2F at = D2D1::Point2F(cx + r * std::sin(rad),
                                             cy - r * std::cos(rad));
            float rr = 5.0f + 15.0f * a.pulse;
            res.brush->SetColor(NeonColor(lvl, cfg, (1.0f - a.pulse) * 0.55f * fx));
            rt->DrawEllipse(D2D1::Ellipse(at, rr, rr), res.brush.Get(), 1.5f);
        }

        // type icon just past the arrow tip (only when classification fires;
        // impacts draw no icon)
        if (classifyOn && classes) {
            uint8_t cls = classes[bestCh];
            if (cls == SoundFootstep || cls == SoundGunshot) {
                D2D1_MATRIX_3X2_F xform =
                    D2D1::Matrix3x2F::Rotation(a.angle, D2D1::Point2F(cx, cy));
                rt->SetTransform(xform);
                float alpha = 0.9f;
                float iy = cy - r - len - 12.0f; // icon center, past the tip
                if (cls == SoundFootstep) {
                    // footprint pair: sole + heel ellipses, mirrored offset;
                    // dark underlay pass keeps them readable on bright scenes
                    auto foot = [&](float dx, float dy, float shade, float a2) {
                        res.brush->SetColor(D2D1::ColorF(shade, shade * 0.72f,
                                                         shade * 0.2f, a2));
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx, iy + dy + 8.5f),
                                                      4.4f, 7.0f), res.brush.Get());
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx, iy + dy + 1.0f),
                                                      3.8f, 3.8f), res.brush.Get());
                    };
                    foot(-4.5f, 1.0f, 0.05f, alpha * 0.55f); // underlay (near-black)
                    foot(4.5f, -1.0f, 0.05f, alpha * 0.55f);
                    foot(-4.5f, 0.0f, 1.0f, alpha);          // amber pair
                    foot(4.5f, -2.0f, 1.0f, alpha);
                } else { // gunshot spark
                    res.brush->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.05f, alpha * 0.55f));
                    rt->SetTransform(D2D1::Matrix3x2F::Scale(1.2f, 1.2f) *
                                     D2D1::Matrix3x2F::Translation(cx + 0.8f, iy + 0.8f) *
                                     xform);
                    rt->FillGeometry(res.star.Get(), res.brush.Get());
                    res.brush->SetColor(D2D1::ColorF(1.0f, 0.30f, 0.20f, alpha));
                    rt->SetTransform(D2D1::Matrix3x2F::Translation(cx, iy) * xform);
                    rt->FillGeometry(res.star.Get(), res.brush.Get());
                }
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }
    }

    // corner disclaimer while classification display is on (tiny, dim)
    if (classifyOn) {
        res.textBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        rt->DrawTextLayout(D2D1::Point2F(8.0f, static_cast<float>(h) - 26.0f),
                           res.cornerLayout.Get(), res.textBrush.Get());
    }

    // edge lines: one sliding border segment per tracked cluster
    for (const auto& a : arrows) {
        float lvl = duckLevel(a.angle, a.strength);
        if (lvl <= 0.02f) continue;
        if (cfg.hideImpact && classifyOn && classes &&
            classes[ArrowNearestChannel(a.angle)] == SoundImpact)
            continue;
        DrawCapsule(rt, res, cfg, a.angle, lvl, a.pulse, w, h, fx, timeSec);
    }
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
        hr = CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), dc.Get(), res);
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
    bool stereoMode_ = false;  // active-channel auto-detect (hysteresis)
    // Experimental view compensation state.
    POINT lastMouse = {};
    bool haveLastMouse = false;
    double turnYawDeg = 0.0;   // accumulated view rotation, degrees
    float modeTimer_ = 0.0f;
    float timeSec_ = 0.0f;     // shimmer clock (advances only while active)
    float duckAmt_ = 0.0f;     // front-duck envelope 0..1 (fire/walk keys)

    // --- render loop: up to ~120 fps while audio active, ~4 fps idle --------
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
                CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), dc.Get(),
                                     res);
                appliedVersion_.store(seenCfgVersion);
            }
        }

        AnalysisFrame frame;
        uint8_t classes[8] = {};
        uint32_t srcCh = 8;
        {
            std::lock_guard<std::mutex> lk(meters_->mu);
            frame = meters_->frame;
            std::memcpy(classes, meters_->classes, sizeof(classes));
            srcCh = meters_->srcChannels;
        }
        // The overlay applies no display gain: the analyzer already applied the
        // adaptive detection gain and the sensitivity curve to frame.level. A
        // second multiply would amplify ambient noise into the display and make
        // the sensitivity slider act twice.
        const DownmixMode downmixMode =
            static_cast<DownmixMode>(g_downmixMode.load());
        float maxLvl = 0.0f;
        for (int c = 0; c < 8; ++c) if (frame.level[c] > maxLvl) maxLvl = frame.level[c];
        bool active = frame.active || maxLvl > 0.02f;

        // direction input: normalized so gain never clips the channel ratio
        // (only rescales when the loudest channel exceeds 1.0)
        float rawLevels[8];
        float dirScale = maxLvl > 1.0f ? 1.0f / maxLvl : 1.0f;
        for (int c = 0; c < 8; ++c) rawLevels[c] = frame.level[c] * dirScale;

        if (!cfg_.mouseTurn) {
            haveLastMouse = false;
            turnYawDeg = 0.0;
        }
        float dirLevels[8];
        if (cfg_.mouseTurn && active) {
            // Turn angle since the previous frame, from the horizontal mouse
            // motion Windows reports. deg/count comes from the calibration, or
            // from cm360 + DPI when the calibration is left at 0.
            double degPerCount = cfg_.mouseDegPerCount;
            if (degPerCount <= 0.0) {
                degPerCount = MouseDegPerCount(cfg_.mouseCm360, cfg_.mouseDpi);
                degPerCount *= cfg_.mouseCalPct / 100.0;
            }
            // GetCursorPos reports DPI-scaled screen pixels, not raw counts.
            // The window DPI tells us the factor without moving the pointer
            // around (moving it every frame would make the cursor jitter).
            static const double scaleFactor = [] {
                const UINT dpi = GetDpiForWindow(GetDesktopWindow());
                const double f = (dpi > 0) ? static_cast<double>(dpi) / 96.0 : 1.0;
                return (f < 0.5 || f > 4.0) ? 1.0 : f;
            }();
            degPerCount /= scaleFactor;
            POINT cur = {};
            if (GetCursorPos(&cur)) {
                if (haveLastMouse) {
                    const double dx = static_cast<double>(cur.x - lastMouse.x);
                    // Ignore a teleport (pointer re-centre, resolution change).
                    if (std::fabs(dx) < 400.0)
                        turnYawDeg += dx * degPerCount * cfg_.mouseTurnSign;
                    turnYawDeg = std::fmod(turnYawDeg, 360.0);
                }
                lastMouse = cur;
                haveLastMouse = true;
            }
            RotateLevels(rawLevels, dirLevels, turnYawDeg, kArrowRingCh, kArrowRingAng);
        } else {
            for (int c = 0; c < 8; ++c) dirLevels[c] = rawLevels[c];
        }

        // direction estimation; mode = stereo-pan vs 8ch cluster
        uint64_t nowTick = GetTickCount64();
        float dt = static_cast<float>(nowTick - lastTick) / 1000.0f;
        lastTick = nowTick;

        // stereo-pan ONLY when BOTH FL and FR carry energy and nothing else
        // does. A single active channel must produce a fixed arc at its own
        // angle (the cluster tracker does that) - never a pan jump to +-90.
        // frontMerge means "one arrow dead ahead", so the pan path stays off:
        // otherwise the merge toggle and the pan path fight over the front pair.
        bool frontPair = frame.level[0] > 0.03f && frame.level[1] > 0.03f;
        bool othersQuiet = true;
        for (int c = 2; c < 8; ++c)
            if (frame.level[c] > 0.03f) othersQuiet = false;
        // A mono downmix puts everything into one ear, so the two output
        // channels are identical, not a stereo pair. Pan tracking would report
        // nonsense (a fixed +-45 deg), so it stays off for the mono modes.
        const bool panCapable = (downmixMode == DownmixStereo);
        bool wantStereo = (srcCh == 2 && panCapable) ||
                          (frontPair && othersQuiet && !cfg_.frontMerge && panCapable);
        if (wantStereo != stereoMode_) {
            // hysteresis: switch only after ~300 ms of consistent evidence
            modeTimer_ += dt;
            if (modeTimer_ > 0.3f) {
                stereoMode_ = wantStereo;
                modeTimer_ = 0.0f;
            }
        } else {
            modeTimer_ = 0.0f;
        }

        // The analyzer reports frame.active only when some channel is above its
        // threshold, so a false value is real silence (not a quiet passage).
        // Clear the arrows at once then; the fade tail is for sound that stops
        // briefly, not for an empty scene.
        const bool silent = !active;
        if (stereoMode_) {
            tracker.UpdateStereo(active ? dirLevels[0] : 0.0f, active ? dirLevels[1] : 0.0f,
                                 dt, cfg_.arrowFadeMs);
        } else {
            float zeros[8] = {};
            tracker.Update(active ? dirLevels : zeros, cfg_.detectThreshold, dt,
                           cfg_.frontMerge, cfg_.arrowFadeMs);
        }
        if (silent) tracker.Clear();
        if (active) timeSec_ += dt; // shimmer clock pauses when idle

        // duck envelope: read-only poll of fire/walk keys, fast attack
        // (~20 ms), linear release over duckReleaseMs
        if (cfg_.duckEnabled) {
            bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool walking = (GetAsyncKeyState('W') & 0x8000) != 0 ||
                           (GetAsyncKeyState('A') & 0x8000) != 0 ||
                           (GetAsyncKeyState('S') & 0x8000) != 0 ||
                           (GetAsyncKeyState('D') & 0x8000) != 0;
            float target = firing ? cfg_.duckFire
                                  : (walking ? cfg_.duckWalk : 0.0f);
            if (target > duckAmt_) {
                duckAmt_ += (target - duckAmt_) * (1.0f - std::exp(-dt / 0.02f));
            } else {
                float relSec = cfg_.duckReleaseMs > 0
                                   ? cfg_.duckReleaseMs * 0.001f : 0.001f;
                duckAmt_ -= dt / relSec;
                if (duckAmt_ < target) duckAmt_ = target;
            }
        } else {
            duckAmt_ = 0.0f;
        }

        {
            std::lock_guard<std::mutex> lk(debugMu_);
            debugAngles_.clear();
            for (const auto& a : tracker.Arrows()) debugAngles_.push_back(a.angle);
        }

        // measure the whole iteration (sleep included) for a true loop CPU%
        CpuProbe probe;
        probe.Begin();
        // 8 ms while audio is present (~120 fps) so arrow motion does not step,
        // 250 ms when idle so an idle overlay costs nothing.
        DWORD timeout = active ? 8 : 250;
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
                DrawScene(dc.Get(), res, cfg_, tracker.Arrows(), timeSec_,
                          duckAmt_, classes, g_classifyEnabled.load(), w, h);
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
        hr = CreateSceneResources(d2dFactory.Get(), dwFactory.Get(), rt.Get(), res);
    if (SUCCEEDED(hr)) {
        // one-shot tracker run (long dt -> snaps to centroids, ripple mid-way)
        ArrowTracker tracker;
        tracker.Update(levels, cfg.detectThreshold, 0.5f, cfg.frontMerge, cfg.arrowFadeMs);
        tracker.Update(levels, cfg.detectThreshold, 0.1f, cfg.frontMerge, cfg.arrowFadeMs);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.06f, 0.06f, 0.09f, 1.0f)); // opaque dark backdrop
        DrawScene(rt.Get(), res, cfg, tracker.Arrows(), 0.35f, 0.0f, classes,
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
