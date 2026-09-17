// overlay.cpp - DirectComposition + Direct2D overlay implementation.
//
// Design (M5 redesign): a thin hollow ring at screen center-bottom - the
// player sees through the middle. Per-channel chevron arrows sit ON the ring
// and point outward at each direction (damage-indicator style, Apex/CS2 HUD).
// Level 0 = invisible; louder = thicker, brighter, plus an outer glow (arrow
// drawn 3x with increasing width / decreasing alpha). Onset ripple: a short
// expanding ring at the arrow position when a channel goes silent -> active.
// Edge bands: thin strips, alpha fading sideways (sliced gradient), low alpha.
// LFE has no direction: it brightens the whole ring.
//
// Why DComp and not UpdateLayeredWindowIndirect: the window is full-screen;
// pushing a per-pixel-alpha 32bpp full-screen bitmap through ULW at 60 fps is
// an ~8 MB CPU copy per frame. With DComp the swapchain is composed by DWM on
// the GPU, so a frame is just a few draw calls + Present.
#include "overlay.h"

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
constexpr float kAngleDeg[8] = { 330.f, 30.f, 0.f, -1.f, 225.f, 135.f, 270.f, 90.f };
constexpr wchar_t kLabel[8][4] = { L"FL", L"FR", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR" };

D2D1_POINT_2F DirFromDeg(float deg) {
    float a = deg * kPi / 180.f;
    return D2D1::Point2F(std::sin(a), -std::cos(a)); // y is down on screen
}

// Green <= low, yellow low..high, red > high; alpha scales with level.
D2D1_COLOR_F LevelColor(float lvl, const OverlayConfig& cfg, float alphaScale = 1.0f) {
    float a = (lvl <= 0.0f) ? 0.0f : (lvl > 1.0f ? 1.0f : lvl);
    a *= alphaScale;
    if (lvl <= cfg.lowThreshold) return D2D1::ColorF(0.20f, 1.00f, 0.30f, a);
    if (lvl <= cfg.highThreshold) return D2D1::ColorF(1.00f, 0.85f, 0.10f, a);
    return D2D1::ColorF(1.00f, 0.15f, 0.10f, a);
}

// --- cached scene resources (rebuilt when the live config version moves) ---

struct SceneResources {
    ComPtr<ID2D1SolidColorBrush> brush;     // arrows/ring/bands
    ComPtr<ID2D1SolidColorBrush> textBrush; // labels
    ComPtr<ID2D1PathGeometry> arrow[8];     // chevron per direction (not LFE)
    D2D1_POINT_2F labelPos[8];
    ComPtr<IDWriteTextLayout> labelLayout[8];
    ComPtr<IDWriteTextLayout> fsLayout;     // "FS" footstep marker
    ComPtr<IDWriteTextLayout> gsLayout;     // "GS" gunshot marker
    ComPtr<IDWriteTextLayout> cornerLayout; // "实验性 Experimental"
};

// Chevron on the ring pointing outward at `deg`: base sits on the ring, tip
// outside it, inner notch makes it read as an arrow.
ComPtr<ID2D1PathGeometry> MakeArrow(ID2D1Factory* factory, float cx, float cy,
                                    float r, float deg) {
    D2D1_POINT_2F d = DirFromDeg(deg);
    D2D1_POINT_2F p = D2D1::Point2F(d.y, -d.x); // perpendicular (rotated -90)
    const float halfW = 9.0f, tipLen = 15.0f, notch = 5.0f;
    D2D1_POINT_2F tip = D2D1::Point2F(cx + (r + tipLen) * d.x, cy + (r + tipLen) * d.y);
    D2D1_POINT_2F b1 = D2D1::Point2F(cx + r * d.x + halfW * p.x, cy + r * d.y + halfW * p.y);
    D2D1_POINT_2F in = D2D1::Point2F(cx + (r + notch) * d.x, cy + (r + notch) * d.y);
    D2D1_POINT_2F b2 = D2D1::Point2F(cx + r * d.x - halfW * p.x, cy + r * d.y - halfW * p.y);

    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return nullptr;
    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(b1);
    sink->AddLine(in);
    sink->AddLine(b2);
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

    ComPtr<IDWriteTextFormat> fmt;
    hr = dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     10.0f, L"", &fmt);
    if (FAILED(hr)) return hr;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    for (int c = 0; c < 8; ++c) {
        hr = dwFactory->CreateTextLayout(kLabel[c], static_cast<UINT32>(wcslen(kLabel[c])),
                                         fmt.Get(), 48.0f, 24.0f, &res.labelLayout[c]);
        if (FAILED(hr)) return hr;
        DWRITE_TEXT_METRICS m = {};
        res.labelLayout[c]->GetMetrics(&m);
        if (c == 3) { // LFE label under the ring center
            res.labelPos[c] = D2D1::Point2F(cx - m.width / 2, cy + 6.0f);
            continue;
        }
        res.arrow[c] = MakeArrow(d2dFactory, cx, cy, r, kAngleDeg[c]);
        if (!res.arrow[c]) return E_FAIL;
        // label just inside the ring at that direction
        D2D1_POINT_2F d = DirFromDeg(kAngleDeg[c]);
        res.labelPos[c] = D2D1::Point2F(cx + r * 0.72f * d.x - m.width / 2,
                                        cy + r * 0.72f * d.y - m.height / 2);
    }
    // classification markers
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

// Edge band as a 5-slice sideways-fading strip: strong at the center of the
// direction's edge segment, fading to the sides. Thin, low alpha.
void DrawBand(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
              float lvl, const D2D1_RECT_F& span, int edge /*0=top 1=right 2=bottom 3=left*/) {
    if (lvl <= 0.02f) return; // zero level = invisible
    const float profile[5] = { 0.20f, 0.55f, 1.0f, 0.55f, 0.20f };
    float t = 2.0f + 14.0f * (lvl > 1.0f ? 1.0f : lvl);
    bool horiz = (edge == 0 || edge == 2);
    float lo = horiz ? span.left : span.top;
    float hi = horiz ? span.right : span.bottom;
    float len = hi - lo;
    for (int i = 0; i < 5; ++i) {
        float a0 = lo + len * i / 5.0f, a1 = lo + len * (i + 1) / 5.0f;
        D2D1_RECT_F r;
        switch (edge) {
            case 0: r = D2D1::RectF(a0, span.top, a1, span.top + t); break;
            case 1: r = D2D1::RectF(span.right - t, a0, span.right, a1); break;
            case 2: r = D2D1::RectF(a0, span.bottom - t, a1, span.bottom); break;
            default: r = D2D1::RectF(span.left, a0, span.left + t, a1); break;
        }
        res.brush->SetColor(LevelColor(lvl, cfg, profile[i] * 0.8f));
        rt->FillRectangle(&r, res.brush.Get());
    }
}

// pulse[c]: 0 = none, else 0..1 progress of the onset ripple (~200 ms).
void DrawScene(ID2D1RenderTarget* rt, const SceneResources& res, const OverlayConfig& cfg,
               const float levels[8], const uint8_t* classes, bool classifyOn,
               const float* pulse, int w, int h) {
    float cx = w * 0.5f + static_cast<float>(cfg.offsetX);
    float cy = h * 0.5f + static_cast<float>(cfg.offsetY);
    float r = static_cast<float>(cfg.radius);
    float fx = cfg.fxPct / 100.0f; // effects intensity 0..1

    float maxLvl = 0.0f;
    for (int c = 0; c < 8; ++c) if (levels[c] > maxLvl) maxLvl = levels[c];

    // hollow ring; LFE brightens it
    float ringAlpha = 0.25f + 0.45f * levels[3];
    float ringStroke = 2.0f + 1.5f * levels[3];
    res.brush->SetColor(D2D1::ColorF(1, 1, 1, ringAlpha));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), res.brush.Get(), ringStroke);

    // tiny center dot only while sound is active
    if (maxLvl > 0.02f) {
        res.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.3f));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.0f, 2.0f), res.brush.Get());
    }

    // direction arrows (never merged: straight from the 8 analyzer levels)
    for (int c = 0; c < 8; ++c) {
        if (c == 3 || !res.arrow[c]) continue;
        float lvl = levels[c];
        if (lvl <= 0.02f) continue;
        float thickness = 1.5f + 2.5f * lvl;
        D2D1_COLOR_F col = LevelColor(lvl, cfg);
        if (fx > 0.0f) {
            // outer glow: two wider, dimmer strokes behind the arrow
            res.brush->SetColor(LevelColor(lvl, cfg, 0.14f * fx));
            rt->DrawGeometry(res.arrow[c].Get(), res.brush.Get(), thickness + 5.0f * fx);
            res.brush->SetColor(LevelColor(lvl, cfg, 0.30f * fx));
            rt->DrawGeometry(res.arrow[c].Get(), res.brush.Get(), thickness + 2.5f * fx);
        }
        res.brush->SetColor(col);
        rt->FillGeometry(res.arrow[c].Get(), res.brush.Get());
        rt->DrawGeometry(res.arrow[c].Get(), res.brush.Get(), thickness);

        // onset ripple at the arrow position
        if (pulse && pulse[c] > 0.0f && fx > 0.0f) {
            float p = pulse[c]; // 0..1
            D2D1_POINT_2F d = DirFromDeg(kAngleDeg[c]);
            D2D1_POINT_2F at = D2D1::Point2F(cx + r * d.x, cy + r * d.y);
            float rr = 4.0f + 16.0f * p;
            res.brush->SetColor(LevelColor(lvl, cfg, (1.0f - p) * 0.5f * fx));
            rt->DrawEllipse(D2D1::Ellipse(at, rr, rr), res.brush.Get(), 1.5f);
        }
    }

    // labels (cached text layouts; dim when the channel is quiet)
    for (int c = 0; c < 8; ++c) {
        float a = 0.30f + 0.55f * (levels[c] > 1.0f ? 1.0f : levels[c]);
        res.textBrush->SetColor(D2D1::ColorF(1, 1, 1, a));
        rt->DrawTextLayout(res.labelPos[c], res.labelLayout[c].Get(), res.textBrush.Get());
    }

    // experimental classification markers: amber FS outline, red GS flash
    if (classifyOn && classes) {
        for (int c = 0; c < 8; ++c) {
            uint8_t cls = classes[c];
            if (cls == SoundNone) continue;
            D2D1_POINT_2F mp = D2D1::Point2F(res.labelPos[c].x, res.labelPos[c].y + 14.0f);
            if (cls == SoundFootstep) {
                res.brush->SetColor(D2D1::ColorF(1.0f, 0.6f, 0.1f, 0.95f));
                if (res.arrow[c])
                    rt->DrawGeometry(res.arrow[c].Get(), res.brush.Get(), 2.0f);
                else
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 30.0f, 30.0f),
                                    res.brush.Get(), 2.0f);
                res.textBrush->SetColor(D2D1::ColorF(1.0f, 0.6f, 0.1f, 0.95f));
                rt->DrawTextLayout(mp, res.fsLayout.Get(), res.textBrush.Get());
            } else if (cls == SoundGunshot) {
                res.brush->SetColor(D2D1::ColorF(1.0f, 0.1f, 0.1f, 0.85f));
                if (res.arrow[c]) {
                    rt->FillGeometry(res.arrow[c].Get(), res.brush.Get()); // red flash
                    rt->DrawGeometry(res.arrow[c].Get(), res.brush.Get(), 3.0f);
                } else {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 26.0f, 26.0f),
                                    res.brush.Get());
                }
                res.textBrush->SetColor(D2D1::ColorF(1.0f, 0.2f, 0.1f, 0.95f));
                rt->DrawTextLayout(mp, res.gsLayout.Get(), res.textBrush.Get());
            }
        }
        // corner disclaimer while classification display is on
        res.textBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.45f));
        rt->DrawTextLayout(D2D1::Point2F(8.0f, static_cast<float>(h) - 26.0f),
                           res.cornerLayout.Get(), res.textBrush.Get());
    }

    // edge bands
    float W = static_cast<float>(w), H = static_cast<float>(h);
    DrawBand(rt, res, cfg, levels[0], D2D1::RectF(0, 0, W / 3, 0), 0);          // top FL
    DrawBand(rt, res, cfg, levels[2], D2D1::RectF(W / 3, 0, 2 * W / 3, 0), 0);  // top C
    DrawBand(rt, res, cfg, levels[1], D2D1::RectF(2 * W / 3, 0, W, 0), 0);      // top FR
    DrawBand(rt, res, cfg, levels[1], D2D1::RectF(W, 0, W, H / 3), 1);          // right FR
    DrawBand(rt, res, cfg, levels[7], D2D1::RectF(W, H / 3, W, 2 * H / 3), 1);  // right SR
    DrawBand(rt, res, cfg, levels[5], D2D1::RectF(W, 2 * H / 3, W, H), 1);      // right BR
    DrawBand(rt, res, cfg, levels[4], D2D1::RectF(0, H, W / 2, H), 2);          // bottom BL
    DrawBand(rt, res, cfg, levels[5], D2D1::RectF(W / 2, H, W, H), 2);          // bottom BR
    DrawBand(rt, res, cfg, levels[0], D2D1::RectF(0, 0, 0, H / 3), 3);          // left FL
    DrawBand(rt, res, cfg, levels[6], D2D1::RectF(0, H / 3, 0, 2 * H / 3), 3);  // left SL
    DrawBand(rt, res, cfg, levels[4], D2D1::RectF(0, 2 * H / 3, 0, H), 3);      // left BL
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

bool Overlay::Start(const OverlayConfig& cfg, SharedMeters* meters, HANDLE quitEvent) {
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
    thread_ = std::thread(&Overlay::ThreadMain, this);
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

void Overlay::ThreadMain() {
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
    // Never visible: created without WS_VISIBLE and never shown. All visual
    // verification goes through --simulate-screenshot (WIC/BMP), not the screen.
    hwnd_.store(hwnd);

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
    if (!initOk)
        fwprintf(stderr, L"overlay: device init failed (hr=0x%08lx)\n",
                 static_cast<unsigned long>(hr));
    uint32_t seenCfgVersion = 0;
    {
        std::lock_guard<std::mutex> lk(g_overlay.mu);
        seenCfgVersion = g_overlay.version;
    }
    appliedVersion_.store(seenCfgVersion);

    // onset ripple state: per-channel level edge detection
    float prevLevel[8] = {};
    float pulse[8] = {};
    uint64_t lastTick = GetTickCount64();

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

        // onset ripple timing (~200 ms), driven by silent->active edges
        uint64_t nowTick = GetTickCount64();
        float dt = static_cast<float>(nowTick - lastTick) / 1000.0f;
        lastTick = nowTick;
        for (int c = 0; c < 8; ++c) {
            bool on = frame.level[c] >= 0.05f;
            if (on && prevLevel[c] < 0.05f) pulse[c] = 0.001f; // start ripple
            else if (pulse[c] > 0.0f) {
                pulse[c] += dt / 0.2f;
                if (pulse[c] >= 1.0f) pulse[c] = 0.0f;
            }
            prevLevel[c] = frame.level[c];
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
                DrawScene(dc.Get(), res, cfg_, frame.level, classes,
                          g_classifyEnabled.load(), pulse, w, h);
                fhr = dc->EndDraw();
                dc->SetTarget(nullptr);
            }
            // DO_NOT_WAIT: the window is hidden, so DWM never consumes frames;
            // a blocking Present would deadlock once the flip queue fills.
            if (SUCCEEDED(fhr)) fhr = swapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (fhr == DXGI_ERROR_WAS_STILL_DRAWING) fhr = S_OK; // dropped frame, fine
            if (SUCCEEDED(fhr)) fhr = dcompDevice->Commit();
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
        // pulse screenshot shows a mid-ripple on active channels
        float pulse[8] = {};
        for (int c = 0; c < 8; ++c)
            if (levels[c] > 0.05f) pulse[c] = 0.35f;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.06f, 0.06f, 0.09f, 1.0f)); // opaque dark backdrop
        DrawScene(rt.Get(), res, cfg, levels, classes, classes != nullptr, pulse,
                  width, height);
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
