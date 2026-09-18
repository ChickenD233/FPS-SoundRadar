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

    // Stereo input (srcChannels == 2): one indicator sweeping the front.
    // pan = (R-L)/(L+R) -> angle = pan * 90 deg, smoothed like the clusters.
    void UpdateStereo(float lvlL, float lvlR, float dt) {
        float smoothA = 1.0f - std::exp(-dt / 0.08f); // 80 ms glide
        float sum = lvlL + lvlR;
        if (sum < 0.05f) { // fade out
            for (size_t k = 0; k < arrows_.size();) {
                arrows_[k].age += dt;
                arrows_[k].strength *= std::exp(-dt / 0.15f);
                if (arrows_[k].strength < 0.02f) {
                    arrows_.erase(arrows_.begin() + k);
                    continue;
                }
                ++k;
            }
            return;
        }
        float pan = (lvlR - lvlL) / sum; // -1..+1
        float target = pan * 90.0f;
        float energy = sum * 0.5f;
        if (arrows_.empty()) {
            Arrow a;
            a.angle = a.trail0 = a.trail1 = target;
            a.strength = energy;
            a.pulse = 0.001f;
            a.matched = true;
            arrows_.push_back(a);
            return;
        }
        if (arrows_.size() > 1) arrows_.resize(1); // stereo = one indicator
        Arrow& ar = arrows_[0];
        ar.matched = true;
        ar.trail1 = ar.trail0;
        ar.trail0 = ar.angle;
        ar.age += dt;
        ar.angle += ShortestDelta(target, ar.angle) * smoothA;
        ar.strength += (energy - ar.strength) * smoothA;
        if (ar.pulse > 0.0f) {
            ar.pulse += dt / 0.15f;
            if (ar.pulse >= 1.0f) ar.pulse = 0.0f;
        }
    }

    static float ShortestDelta(float a, float b) { // a-b in -180..180
        return std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    }

private:
    std::vector<Arrow> arrows_;
};

// Neon sonar palette: cool cyan->teal below the low threshold, teal->amber up
// to the high threshold, amber->red-magenta above. Threshold config semantics
// unchanged. Alpha floor ~0.75 while active (readability).
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
    float a = (lvl <= 0.0f) ? 0.0f : (0.75f + 0.25f * cl);
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

// Arc band at `radius`, `spanDeg` wide, centered pointing up (0 deg).
// Open figure; drawn with round caps. Built per frame (span animates).
ComPtr<ID2D1PathGeometry> MakeArc(ID2D1Factory* factory, float cx, float cy,
                                  float r, float spanDeg) {
    float half = spanDeg * kPi / 360.0f;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return nullptr;
    sink->BeginFigure(D2D1::Point2F(cx - r * std::sin(half), cy - r * std::cos(half)),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT arc = {};
    arc.point = D2D1::Point2F(cx + r * std::sin(half), cy - r * std::cos(half));
    arc.size = D2D1::SizeF(r, r);
    arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arc.arcSize = D2D1_ARC_SIZE_SMALL;
    sink->AddArc(&arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

// (chevron geometry removed: arcs are built per frame by MakeArc)

HRESULT CreateSceneResources(ID2D1Factory* d2dFactory, IDWriteFactory* dwFactory,
                             ID2D1RenderTarget* rt, const OverlayConfig& cfg,
                             int w, int h, SceneResources& res) {
    (void)cfg; (void)w; (void)h; // arcs are built per frame; nothing layout-bound
    res.factory = d2dFactory;
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

// Directional capsule on the screen border: project the tracked angle onto
// the rounded-rect border (inset 10 px), draw a short glowing segment there,
// tangent to the border. Slides along the edge as the angle moves.
void DrawCapsule(ID2D1RenderTarget* rt, const SceneResources& res,
                 const OverlayConfig& cfg, float angleDeg, float lvl, int w, int h,
                 float fx) {
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

    float len = 120.0f * (0.35f + 0.65f * lvl); // ~120 px scaled by level
    float thick = 6.0f;
    D2D1_COLOR_F col = NeonColor(lvl, cfg);

    auto seg = [&](float halfLen, float t, float alphaMul) {
        ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(res.factory->CreatePathGeometry(&geo))) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink))) return;
        sink->BeginFigure(D2D1::Point2F(hx - tx * halfLen, hy - ty * halfLen),
                          D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(hx + tx * halfLen, hy + ty * halfLen));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        res.brush->SetColor(NeonColor(lvl, cfg, alphaMul));
        rt->DrawGeometry(geo.Get(), res.brush.Get(), t, res.roundCaps.Get());
    };

    if (fx > 0.0f) seg(len * 0.5f, thick + 10.0f * fx, 0.22f * fx); // glow
    // main capsule: gradient dim->bright along its length
    ComPtr<ID2D1PathGeometry> geo;
    if (SUCCEEDED(res.factory->CreatePathGeometry(&geo))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(hx - tx * len * 0.5f, hy - ty * len * 0.5f),
                              D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(hx + tx * len * 0.5f, hy + ty * len * 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
        }
        D2D1_COLOR_F dim = col, bright = col;
        dim.r *= 0.50f; dim.g *= 0.50f; dim.b *= 0.50f;
        bright.r += (1.0f - bright.r) * 0.25f;
        bright.g += (1.0f - bright.g) * 0.25f;
        bright.b += (1.0f - bright.b) * 0.25f;
        D2D1_GRADIENT_STOP stops[2];
        stops[0].position = 0.0f; stops[0].color = dim;
        stops[1].position = 1.0f; stops[1].color = bright;
        ComPtr<ID2D1GradientStopCollection> gsc;
        rt->CreateGradientStopCollection(stops, 2, &gsc);
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (gsc)
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(hx - tx * len * 0.5f, hy - ty * len * 0.5f),
                    D2D1::Point2F(hx + tx * len * 0.5f, hy + ty * len * 0.5f)),
                gsc.Get(), &grad);
        if (grad)
            rt->DrawGeometry(geo.Get(), grad.Get(), thick, res.roundCaps.Get());
        else {
            res.brush->SetColor(col);
            rt->DrawGeometry(geo.Get(), res.brush.Get(), thick, res.roundCaps.Get());
        }
    }
}

// Neon sonar scene: floating luminous arc segments around an invisible
// center, gradient-filled, with glow, inner highlight, motion trail, onset
// ripple, and crisp vector type icons (footstep pair / gunshot spark).
void DrawScene(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
               const float levels[8], const std::vector<ArrowTracker::Arrow>& arrows,
               const uint8_t* classes, bool classifyOn, int w, int h) {
    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float r = static_cast<float>(cfg.radius);
    float fx = cfg.fxPct / 100.0f;
    (void)levels; // everything direction-related reads the tracked clusters now

    for (const auto& a : arrows) {
        if (a.strength <= 0.02f) continue;
        float lvl = a.strength;
        // scale-in pop over ~150 ms after spawn
        float pop = (a.age < 0.15f) ? 0.6f + 0.4f * (a.age / 0.15f) : 1.0f;
        float span = (34.0f + 26.0f * lvl) * pop;      // 34..60 deg
        float thick = (10.0f + 9.0f * lvl) * pop;      // ~10px base, grows
        D2D1_COLOR_F col = NeonColor(lvl, cfg);

        auto drawArc = [&](float deg, float spanD, float t, float alphaMul) {
            ComPtr<ID2D1PathGeometry> geo = MakeArc(res.factory.Get(), cx, cy, r, spanD);
            if (!geo) return;
            rt->SetTransform(D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(cx, cy)));
            res.brush->SetColor(NeonColor(lvl, cfg, alphaMul));
            rt->DrawGeometry(geo.Get(), res.brush.Get(), t, res.roundCaps.Get());
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        };

        // motion trail: two fading arcs at recent older angles
        if (fx > 0.0f) {
            const float trails[2] = { a.trail1, a.trail0 };
            const float trailA[2] = { 0.10f, 0.20f };
            for (int ti = 0; ti < 2; ++ti) {
                if (trails[ti] == a.angle) continue;
                drawArc(trails[ti], span * 0.85f, thick * 0.75f, trailA[ti] * fx);
            }
        }
        // soft outer glow (two passes)
        if (fx > 0.0f) {
            drawArc(a.angle, span, thick + 10.0f * fx, 0.16f * fx);
            drawArc(a.angle, span, thick + 5.0f * fx, 0.34f * fx);
        }
        // main arc: gradient fill (dimmer tail -> bright head, along the arc)
        {
            ComPtr<ID2D1PathGeometry> geo = MakeArc(res.factory.Get(), cx, cy, r, span);
            if (geo) {
                float halfLen = r * span * kPi / 360.0f; // half arc length
                D2D1_GRADIENT_STOP stops[2];
                D2D1_COLOR_F dim = col, bright = col;
                dim.r *= 0.50f; dim.g *= 0.50f; dim.b *= 0.50f;
                // head shifts toward deeper saturation, NOT toward white
                // (whitening disappears on bright backgrounds)
                auto clampf = [](float v) { return v > 1.0f ? 1.0f : v; };
                bright.r = clampf(bright.r * 1.25f);
                bright.g = clampf(bright.g * 1.25f);
                bright.b = clampf(bright.b * 1.25f);
                stops[0].position = 0.0f; stops[0].color = dim;
                stops[1].position = 1.0f; stops[1].color = bright;
                ComPtr<ID2D1GradientStopCollection> gsc;
                rt->CreateGradientStopCollection(stops, 2, &gsc);
                ComPtr<ID2D1LinearGradientBrush> grad;
                if (gsc)
                    rt->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(cx - halfLen, cy - r),
                            D2D1::Point2F(cx + halfLen, cy - r)),
                        gsc.Get(), &grad);
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(a.angle, D2D1::Point2F(cx, cy)));
                if (grad)
                    rt->DrawGeometry(geo.Get(), grad.Get(), thick, res.roundCaps.Get());
                else
                    rt->DrawGeometry(geo.Get(), res.brush.Get(), thick, res.roundCaps.Get());
                // 1px inner highlight line
                ComPtr<ID2D1PathGeometry> hi =
                    MakeArc(res.factory.Get(), cx, cy, r - thick * 0.5f + 0.5f, span * 0.92f);
                if (hi) {
                    res.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
                    rt->DrawGeometry(hi.Get(), res.brush.Get(), 1.0f, res.roundCaps.Get());
                }
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }

        // onset ripple flash at the arc position
        if (a.pulse > 0.0f && fx > 0.0f) {
            float rad = a.angle * kPi / 180.0f;
            D2D1_POINT_2F at = D2D1::Point2F(cx + r * std::sin(rad),
                                             cy - r * std::cos(rad));
            float rr = 4.0f + 14.0f * a.pulse;
            res.brush->SetColor(NeonColor(lvl, cfg, (1.0f - a.pulse) * 0.5f * fx));
            rt->DrawEllipse(D2D1::Ellipse(at, rr, rr), res.brush.Get(), 1.5f);
        }

        // type icon just inside the arc, only when classification fires
        if (classifyOn && classes) {
            int bestCh = -1;
            float bestD = 1e9f;
            for (int c = 0; c < 8; ++c) {
                if (c == 3) continue; // LFE has no angle
                float d = std::fabs(AngDist(a.angle, kAngleDeg[c]));
                if (d < bestD) { bestD = d; bestCh = c; }
            }
            uint8_t cls = bestCh >= 0 ? classes[bestCh] : SoundNone;
            if (cls != SoundNone) {
                D2D1_MATRIX_3X2_F xform =
                    D2D1::Matrix3x2F::Rotation(a.angle, D2D1::Point2F(cx, cy));
                rt->SetTransform(xform);
                float alpha = 0.9f * pop;
                float iy = cy - r + 24.0f; // icon center (inside the arc)
                if (cls == SoundFootstep) {
                    // footprint pair: sole + heel ellipses, mirrored offset;
                    // dark underlay pass keeps them readable on bright scenes
                    auto foot = [&](float dx, float dy, float shade, float a) {
                        res.brush->SetColor(D2D1::ColorF(shade, shade * 0.72f,
                                                         shade * 0.2f, a));
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

    // edge capsules: one sliding border segment per tracked cluster
    for (const auto& a : arrows) {
        if (a.strength <= 0.02f) continue;
        DrawCapsule(rt, res, cfg, a.angle, a.strength, w, h, fx);
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
        uint32_t srcCh = 8;
        {
            std::lock_guard<std::mutex> lk(meters_->mu);
            frame = meters_->frame;
            std::memcpy(classes, meters_->classes, sizeof(classes));
            srcCh = meters_->srcChannels;
        }
        float maxLvl = 0.0f;
        for (int c = 0; c < 8; ++c) if (frame.level[c] > maxLvl) maxLvl = frame.level[c];
        bool active = frame.active || maxLvl > 0.02f;

        // direction estimation
        uint64_t nowTick = GetTickCount64();
        float dt = static_cast<float>(nowTick - lastTick) / 1000.0f;
        lastTick = nowTick;
        if (srcCh == 2) {
            // stereo input: pan sweeps one indicator across the front
            if (active) tracker.UpdateStereo(frame.level[0], frame.level[1], dt);
            else tracker.UpdateStereo(0.0f, 0.0f, dt); // fade out
        } else if (active) {
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
