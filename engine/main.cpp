// main.cpp - SoundRadar engine: capture -> analyze -> downmix -> render,
// plus overlay window, tray menu, GUI main window, and headless test modes.
//
// Thread model:
//   capture thread : WASAPI packet -> analysis (8ch, independent) -> ring
//   render thread  : ring -> Downmix8To2 (live config snapshot) -> submit
//   overlay thread : SharedMeters snapshot -> DComp/D2D ring + arrows
//   main thread    : GUI + tray message loop, config, shutdown
#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../overlay/overlay.h"
#include "analysis.h"
#include "capture.h"
#include "classify.h"
#include "config.h"
#include "devicedefault.h"
#include "downmix.h"
#include "gui2.h"
#include "log.h"
#include "measure.h"
#include "meters.h"
#include "pantest.h"
#include "render.h"
#include "ring.h"
#include "selftest.h"
#include "simulate.h"
#include "tray.h"

namespace {

HANDLE g_quit = nullptr;
HANDLE g_singleton = nullptr;

BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (g_quit) SetEvent(g_quit);
            return TRUE;
        default:
            return FALSE;
    }
}

// Single instance: second launch activates the existing main window, exits.
bool EnsureSingleInstance() {
    g_singleton = CreateMutexW(nullptr, TRUE, L"Local\\SoundRadarSingleton");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
    HWND found = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, sr::kGuiClassName) == 0) {
                *reinterpret_cast<HWND*>(lp) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    if (found) {
        PostMessageW(found, sr::kMsgGuiActivate, 0, 0);
        sr::Log("second instance: activated existing window, exiting");
    } else {
        sr::Log("second instance: existing process has no window yet, exiting");
    }
    return false;
}

void PrintUsage() {
    std::printf(
        "SoundRadar - 7.1 capture, metering, right-mono/stereo downmix, overlay\n"
        "\n"
        "usage: SoundRadar.exe [options]\n"
        "  (no args)              main window + tray + engine\n"
        "  --tray                 tray only, no window (autostart uses this)\n"
        "  --selftest             DSP self-tests (no audio devices needed), exit 0/1\n"
        "  --classifytest         sound classification tests (experimental), exit 0/1\n"
        "  --guitest              hidden GUI test: controls, Apply, config round-trip\n"
        "  --simulate-gui         hidden GUI + overlay plumbing test\n"
        "  --onscreen-proof       visible overlay + CopyFromScreen pixel check\n"
        "  --simulate <scenario>  sweep | dual | pulse | orbit | dual-orbit\n"
        "  --simulate-screenshot <file.bmp>  render one dual frame to a BMP\n"
        "  --overlaytest [bmp]    overlay checks: ex-style, CPU, screenshots\n"
        "  --measure              latency report (needs SoundRadar driver + output)\n"
        "  --measure-loopback     click-train round-trip through the SoundRadar driver\n"
        "  --pan-test [seconds]   play per-channel test tones on the SoundRadar speaker\n"
        "  --set-default          make the capture device's render twin the default output\n"
        "  --list-devices         list capture and render endpoints\n"
        "  --output <name>        render endpoint name substring\n"
        "  --mode <m>             right-mono | stereo (overrides config)\n"
        "  --config <path>        config file (default %%APPDATA%%\\SoundRadar\\config.json)\n"
        "  --help                 this text\n");
}

void ListDevices(const std::wstring& captureDevice) {
    sr::ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return;
    }
    std::vector<sr::DeviceInfo> candidates = sr::SelectCaptureEndpoints(captureDevice);
    std::wstring selectedId = candidates.empty() ? L"" : candidates.front().id;

    std::printf("capture endpoints:\n");
    for (const auto& d : sr::EnumerateEndpoints(eCapture))
        std::printf("  %s%s%s\n", sr::ToUtf8(d.name).c_str(),
                    d.isDefault ? "  [default]" : "",
                    (!selectedId.empty() && d.id == selectedId) ? "  (selected)" : "");
    if (selectedId.empty())
        std::printf("  (no endpoint matches the capture_device rule)\n");
    std::printf("render endpoints:\n");
    for (const auto& d : sr::EnumerateEndpoints(eRender))
        std::printf("  %s%s%s\n", sr::ToUtf8(d.name).c_str(),
                    d.isDefault ? "  [default]" : "",
                    sr::IsVirtualAudioName(d.name) ? "  (虚拟)" : "");
}

void PrintMeterLine(const sr::AnalysisFrame& fr) {
    static const char* names[sr::kChannels] = { "FL", "FR", "C ", "LFE", "BL", "BR", "SL", "SR" };
    std::printf("\r");
    for (int c = 0; c < sr::kChannels; ++c)
        std::printf("%s %.2f%s  ", names[c], fr.level[c], fr.peak[c] ? "*" : " ");
    std::printf("%s   ", fr.active ? "ACTIVE" : "idle  ");
    std::fflush(stdout);
}

// --- restartable audio pipeline ----------------------------------------------

struct Pipeline {
    bool running = false;
    std::wstring capName, renName, lastError;
    bool exclusive = false;
    double capBufMs = 0, renBufMs = 0, periodMs = 0;

    bool Start(const sr::AppConfig& cfg, sr::SharedMeters* meters) {
        Stop();
        lastError.clear();
        cap_ = std::make_unique<sr::CaptureClient>();
        if (!cap_->Init(cfg.captureDevice, lastError)) {
            sr::Log("pipeline: capture init failed: %s", sr::ToUtf8(lastError).c_str());
            cap_.reset();
            return false;
        }
        ren_ = std::make_unique<sr::RenderClient>();
        if (!ren_->Init(cfg.outputDevice, cfg.renderExclusive, lastError)) {
            sr::Log("pipeline: render init failed: %s", sr::ToUtf8(lastError).c_str());
            cap_.reset();
            ren_.reset();
            return false;
        }
        capName = cap_->DeviceName();
        renName = ren_->DeviceName();
        exclusive = ren_->GetFormat().exclusive;
        capBufMs = cap_->GetFormat().bufferFrames * 1000.0 / cap_->GetFormat().sampleRate;
        renBufMs = ren_->GetFormat().bufferMs;
        periodMs = cap_->GetFormat().periodMs;
        { // publish the real channel count: 2ch input switches the overlay to pan tracking
            std::lock_guard<std::mutex> lk(meters->mu);
            meters->srcChannels = cap_->GetFormat().channels;
        }
        sr::Log("pipeline: capture=%s render=%s (%s)",
                sr::ToUtf8(capName).c_str(), sr::ToUtf8(renName).c_str(),
                exclusive ? "exclusive" : "shared");

        ring_ = std::make_unique<sr::RingBuffer>(8192);
        sr::AnalysisConfig acfg = cfg.analysis;
        acfg.sampleRate = static_cast<float>(cap_->GetFormat().sampleRate);
        analyzer_ = std::make_unique<sr::Analyzer>(acfg);
        sr::ClassifyConfig ccfg;
        ccfg.sampleRate = static_cast<float>(cap_->GetFormat().sampleRate);
        classifier_ = std::make_unique<sr::Classifier8>(ccfg);
        meters_ = meters;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        running = true;

        sr::CaptureClient* cap = cap_.get();
        sr::Analyzer* analyzer = analyzer_.get();
        sr::Classifier8* classifier = classifier_.get();
        sr::RingBuffer* ring = ring_.get();
        capThread_ = std::thread([=] {
            uint32_t seenVer = 0;
            {
                std::lock_guard<std::mutex> lk(sr::g_analysis.mu);
                seenVer = sr::g_analysis.version;
            }
            cap->Run([&](const float* frames, uint32_t n, LONGLONG) {
                { // hot-apply analysis config (GUI fade slider)
                    std::lock_guard<std::mutex> lk(sr::g_analysis.mu);
                    if (sr::g_analysis.version != seenVer) {
                        analyzer->SetConfig(sr::g_analysis.cfg);
                        seenVer = sr::g_analysis.version;
                    }
                }
                sr::AnalysisFrame fr;
                analyzer->Process(frames, n, fr);
                classifier->Process(frames, n);
                {
                    std::lock_guard<std::mutex> lk(meters->mu);
                    meters->frame = fr;
                    for (int c = 0; c < 8; ++c)
                        meters->classes[c] = static_cast<uint8_t>(classifier->ClassOf(c));
                }
                ring->Write(frames, n);
            }, stopEvent_);
        });

        sr::RenderClient* ren = ren_.get();
        renThread_ = std::thread([=] {
            std::vector<float> in8, stereo;
            ren->Run([&](float* out, uint32_t frames) {
                size_t need8 = static_cast<size_t>(frames) * 8;
                if (in8.size() < need8) {
                    in8.resize(need8);
                    stereo.resize(static_cast<size_t>(frames) * 2);
                }
                size_t got = ring->Read(in8.data(), frames);
                if (got < frames)
                    std::memset(in8.data() + got * 8, 0, (frames - got) * 8 * sizeof(float));
                sr::DownmixConfig d;
                {
                    std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
                    d = sr::g_downmix.cfg; // live snapshot (tray/GUI Apply)
                }
                sr::Downmix8To2(in8.data(), stereo.data(), frames, d);
                std::memcpy(out, stereo.data(), frames * 2 * sizeof(float));
            }, stopEvent_);
        });
        return true;
    }

    void Stop() {
        running = false;
        if (stopEvent_) SetEvent(stopEvent_);
        if (capThread_.joinable()) capThread_.join();
        if (renThread_.joinable()) renThread_.join();
        cap_.reset();
        ren_.reset();
        analyzer_.reset();
        classifier_.reset();
        ring_.reset();
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    ~Pipeline() { Stop(); }

    // Glitch telemetry: logs one line when any counter is non-zero (deltas
    // since the previous call). Called ~1/s from the tray tick.
    void PollTelemetry() {
        if (!running) return;
        uint64_t ringOver = ring_ ? ring_->Overruns() : 0;
        uint64_t ringDelta = ringOver - lastRingOverruns_;
        lastRingOverruns_ = ringOver;
        uint32_t pMin = 0, pMax = 0;
        if (ren_) ren_->GetPaddingStats(pMin, pMax);
        uint64_t gaps = cap_ ? cap_->GetPacketGaps() : 0;
        bool padJitter = (pMax > pMin); // padding moved at all within the second
        if (ringDelta || gaps || (pMax && padJitter))
            sr::Log("telemetry: ringOverruns=+%llu renderPad=[%lu..%lu] capGaps=+%llu",
                    (unsigned long long)ringDelta, (unsigned long)pMin,
                    (unsigned long)pMax, (unsigned long long)gaps);
    }

private:
    uint64_t lastRingOverruns_ = 0;
    std::unique_ptr<sr::CaptureClient> cap_;
    std::unique_ptr<sr::RenderClient> ren_;
    std::unique_ptr<sr::RingBuffer> ring_;
    std::unique_ptr<sr::Analyzer> analyzer_;
    std::unique_ptr<sr::Classifier8> classifier_;
    sr::SharedMeters* meters_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread capThread_, renThread_;
};

// Full app: pipeline + overlay + GUI + tray. showGui=false is --tray mode.
int RunApp(sr::AppConfig& cfg, const std::wstring& configPath, bool trayMode,
           bool showGui) {
    sr::ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }

    // publish config to the live snapshots
    {
        std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
        sr::g_downmix.cfg = cfg.downmix;
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_analysis.mu);
        sr::g_analysis.cfg = cfg.analysis;
        ++sr::g_analysis.version;
    }
    sr::g_classifyEnabled.store(cfg.classifyEnabled);

    sr::SharedMeters meters;
    Pipeline pipeline;

    sr::Overlay overlay;
    if (cfg.overlay.enabled) {
        overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/true);
        sr::Log("overlay: started (enabled)");
    }

    sr::Tray tray;
    sr::Gui gui;
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.onExit = [&] { SetEvent(g_quit); };
    hooks.statusLevel = [&] {
        return pipeline.running ? 0 : (pipeline.lastError.empty() ? 1 : 2);
    };
    hooks.levels = [&] {
        std::array<float, 8> l{};
        std::lock_guard<std::mutex> lk(meters.mu);
        std::memcpy(l.data(), meters.frame.level, sizeof(l));
        return l;
    };
    hooks.statusText = [&]() -> std::wstring {
        if (!pipeline.running)
            return L"等待设备 Waiting for device\r\n" + pipeline.lastError;
        // Live levels so the user can see whether audio reaches the engine.
        static const wchar_t* kNames[8] = { L"FL", L"FR", L"C", L"LFE",
                                            L"BL", L"BR", L"SL", L"SR" };
        wchar_t lv[256] = {};
        int off = 0;
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            for (int c = 0; c < 8; ++c)
                off += swprintf_s(lv + off, 256 - off, L"%s %.2f  ",
                                  kNames[c], meters.frame.level[c]);
        }
        wchar_t buf[1400];
        double latency = pipeline.capBufMs + pipeline.periodMs + pipeline.renBufMs;
        swprintf_s(buf, L"运行中 Running\r\n输入 In: %s\r\n输出 Out: %s (%s)\r\n估计延迟 Latency ~%.0f ms\r\n电平 Levels: %s",
                   pipeline.capName.c_str(), pipeline.renName.c_str(),
                   pipeline.exclusive ? L"独占 exclusive" : L"共享 shared", latency, lv);
        return buf;
    };
    hooks.onApply = [&](bool devChanged) {
        if (devChanged) {
            sr::Log("gui: device change, restarting pipeline");
            pipeline.Start(cfg, &meters);
            if (!pipeline.running) tray.Balloon(pipeline.lastError);
        }
    };

    if (!gui.Create(hooks, /*hidden=*/!showGui)) {
        sr::Log("gui: creation failed, tray-only fallback");
        tray.Balloon(L"主界面创建失败，仅以托盘运行 (GUI failed, tray only)");
    }

    sr::Tray::Handlers handlers;
    handlers.onOpenGui = [&] { gui.Show(); };
    handlers.onMode = [&](int m) {
        {
            std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
            sr::g_downmix.cfg.mode = static_cast<sr::DownmixMode>(m);
        }
        cfg.downmix.mode = static_cast<sr::DownmixMode>(m);
        sr::SaveConfig(configPath, cfg);
    };
    handlers.onOverlay = [&](bool on) {
        // Overlay off = render thread fully stopped, resources destroyed.
        // The audio path is untouched either way.
        if (on) overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/true);
        else overlay.Stop();
        cfg.overlay.enabled = on;
        sr::SaveConfig(configPath, cfg);
    };
    handlers.onClassify = [&](bool on) {
        sr::g_classifyEnabled.store(on);
        cfg.classifyEnabled = on;
        sr::SaveConfig(configPath, cfg);
    };
    handlers.onAutostart = [&](bool on) {
        cfg.autostart = on;
        sr::SaveConfig(configPath, cfg);
    };
    handlers.onExit = [&] { SetEvent(g_quit); };
    handlers.onTick = [&] {
        // ~1 s glitch telemetry (logged only when non-zero)
        static int tick = 0;
        if (++tick % 2 == 0) pipeline.PollTelemetry();
        if (!GetConsoleWindow()) return;
        sr::AnalysisFrame fr;
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            fr = meters.frame;
        }
        PrintMeterLine(fr);
    };
    tray.Init(cfg, handlers);

    pipeline.Start(cfg, &meters);
    if (!pipeline.running) {
        sr::Log("pipeline: initial start failed, waiting for GUI Apply");
        if (trayMode) tray.Balloon(pipeline.lastError);
    }

    tray.Run(g_quit); // message loop drives both tray and GUI windows

    sr::Log("shutdown");
    if (gui.Hwnd()) DestroyWindow(gui.Hwnd());
    tray.Shutdown();
    overlay.Stop();
    pipeline.Stop();
    return 0;
}

bool CheckExStyle(HWND hwnd) {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    struct Flag { DWORD bits; const char* name; };
    const Flag req[] = {
        { WS_EX_TRANSPARENT, "WS_EX_TRANSPARENT" },
        { WS_EX_LAYERED, "WS_EX_LAYERED" },
        { WS_EX_TOPMOST, "WS_EX_TOPMOST" },
        { WS_EX_NOACTIVATE, "WS_EX_NOACTIVATE" },
        { WS_EX_TOOLWINDOW, "WS_EX_TOOLWINDOW" },
    };
    std::printf("overlay window ex-style: 0x%08lx\n", static_cast<unsigned long>(ex));
    bool ok = true;
    for (const Flag& f : req) {
        bool set = (ex & f.bits) != 0;
        std::printf("  %-18s : %s\n", f.name, set ? "set" : "MISSING");
        if (!set) ok = false;
    }
    return ok;
}

HWND WaitForOverlayWindow(const sr::Overlay& overlay, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        HWND hwnd = overlay.Hwnd();
        if (hwnd) return hwnd;
        if (WaitForSingleObject(g_quit, 0) == WAIT_OBJECT_0) return nullptr;
        Sleep(50);
    }
    return nullptr;
}

int RunSimulate(sr::AppConfig cfg, sr::SimScenario scenario) {
    cfg.overlay.enabled = true;
    sr::SharedMeters meters;
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/false); // headless test

    HWND hwnd = WaitForOverlayWindow(overlay, 3000);
    bool styleOk = false;
    if (!hwnd) {
        std::printf("[FAIL] overlay window was not created\n");
    } else {
        styleOk = CheckExStyle(hwnd);
    }

    sr::Simulator sim(scenario, &meters, g_quit);
    sim.Start();

    Sleep(1200);
    uint64_t f0 = overlay.FramesDrawn();
    Sleep(1200);
    uint64_t f1 = overlay.FramesDrawn();
    bool alive = overlay.IsRunning() && f1 > f0;
    std::printf("render thread alive: %s (%llu frames drawn in 1.2 s window)\n",
                alive ? "yes" : "NO", (unsigned long long)(f1 - f0));

    for (int i = 0; i < 90 && WaitForSingleObject(g_quit, 100) == WAIT_TIMEOUT; ++i) {
    }

    sim.Stop();
    overlay.Stop();
    bool ok = styleOk && alive;
    std::printf("simulate: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int RunOverlayTest(sr::AppConfig cfg, const std::wstring& shotPath) {
    cfg.overlay.enabled = true;
    sr::SharedMeters meters;
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/false); // headless test

    HWND hwnd = WaitForOverlayWindow(overlay, 3000);
    bool styleOk = hwnd && CheckExStyle(hwnd);

    sr::Simulator sim(sr::SimDual, &meters, g_quit);
    sim.Start();

    Sleep(1500);
    overlay.ResetStats();
    Sleep(4000);
    sr::Overlay::Stats active = overlay.GetStats();

    sim.SetSilent(true);
    Sleep(2000);
    overlay.ResetStats();
    Sleep(3000);
    sr::Overlay::Stats idle = overlay.GetStats();

    std::printf("overlay render thread CPU: active %.3f%% (%llu frames in window), "
                "idle %.3f%%\n",
                active.activeCpuPct, (unsigned long long)active.frames, idle.idleCpuPct);

    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    auto baseName = [&](const wchar_t* tag) {
        size_t dot = shotPath.find_last_of(L'.');
        std::wstring stem = dot == std::wstring::npos ? shotPath : shotPath.substr(0, dot);
        return stem + L"-" + tag + L".bmp";
    };
    bool shot = true;
    {
        float dualLevels[8] = {};
        dualLevels[0] = 0.8f;
        dualLevels[5] = 0.8f;
        uint8_t cls[8] = {};
        cls[0] = sr::SoundFootstep;
        cls[5] = sr::SoundGunshot;
        std::wstring p = baseName(L"dual");
        bool ok = sr::RenderSceneToFile(p, w, h, dualLevels, cls, cfg.overlay);
        std::printf("screenshot: %s (%s)\n", sr::ToUtf8(p).c_str(), ok ? "written" : "FAILED");
        shot = shot && ok;
    }
    {
        float sweepLevels[8] = {};
        sweepLevels[7] = 0.85f;
        std::wstring p = baseName(L"sweep");
        bool ok = sr::RenderSceneToFile(p, w, h, sweepLevels, nullptr, cfg.overlay);
        std::printf("screenshot: %s (%s)\n", sr::ToUtf8(p).c_str(), ok ? "written" : "FAILED");
        shot = shot && ok;
    }
    {
        float pulseLevels[8] = {};
        pulseLevels[6] = 0.9f;
        uint8_t cls[8] = {};
        cls[6] = sr::SoundFootstep;
        std::wstring p = baseName(L"pulse");
        bool ok = sr::RenderSceneToFile(p, w, h, pulseLevels, cls, cfg.overlay);
        std::printf("screenshot: %s (%s)\n", sr::ToUtf8(p).c_str(), ok ? "written" : "FAILED");
        shot = shot && ok;
    }

    sim.Stop();
    overlay.Stop();
    bool cpuOk = active.activeCpuPct < 5.0 && idle.idleCpuPct < 2.0;
    bool framesOk = active.frames > 100;
    bool ok = styleOk && cpuOk && framesOk && shot;
    std::printf("overlaytest: style=%s cpu=%s frames=%s screenshot=%s -> %s\n",
                styleOk ? "ok" : "FAIL", cpuOk ? "ok" : "FAIL", framesOk ? "ok" : "FAIL",
                shot ? "ok" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// --- hidden GUI tests ----------------------------------------------------------

std::wstring TempConfigPath(const wchar_t* name) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

struct GuiTestCtx {
    int lines = 0;
};

// Renders a (possibly hidden) window to a 24-bit BMP via PrintWindow.
bool SnapshotWindowToBmp(HWND hwnd, const std::wstring& path) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    // hidden windows never paint; park the window fully off-screen and show
    // it (invisible to the user), so PrintWindow gets real content
    SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNA);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    MSG m;
    while (PeekMessageW(&m, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    // WM_PRINT renders the window tree ourselves - reliable without any DWM
    // surface (PrintWindow returns black for off-screen/hidden windows here).
    SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                 PRF_CHILDREN | PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    BOOL ok = TRUE;
    ShowWindow(hwnd, SW_HIDE);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    size_t rowStride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3); // DIB rows are 4-aligned
    px.assign(rowStride * h, 0);
    int rows = GetDIBits(mem, bmp, 0, h, px.data(), reinterpret_cast<BITMAPINFO*>(&bi),
                         DIB_RGB_COLORS);
    if (rows == 0) { /* fall through to file write failure below */ }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok || rows == 0) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfSize = sizeof(fh) + sizeof(bi) + (uint32_t)px.size();
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    BITMAPINFOHEADER bih = bi;
    bih.biHeight = h;
    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&bih, 1, sizeof(bih), f);
    std::vector<uint8_t> row(rowStride);
    for (int r = h - 1; r >= 0; --r) {
        std::memcpy(row.data(), px.data() + static_cast<size_t>(r) * rowStride,
                    rowStride);
        fwrite(row.data(), 1, rowStride, f);
    }
    fclose(f);
    return true;
}

int RunGuiTest() {
    sr::ComInit com;
    if (!com.Ok()) return 1;

    sr::AppConfig cfg; // defaults
    std::wstring configPath = TempConfigPath(L"soundradar_guitest_config.json");
    DeleteFileW(configPath.c_str());

    int applyCalls = 0;
    bool lastDevChanged = false;
    HANDLE testQuit = CreateEventW(nullptr, TRUE, FALSE, nullptr); // exit cmd target
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.onApply = [&](bool devChanged) {
        ++applyCalls;
        lastDevChanged = devChanged;
    };
    hooks.onExit = [&] { SetEvent(testQuit); };
    hooks.statusLevel = [] { return 1; };
    hooks.statusText = [] { return std::wstring(L"test status"); };
    hooks.levels = [] { return std::array<float, 8>{}; };

    bool ok = true;
    auto check = [&](const char* name, bool pass) {
        std::printf("  [%s] %s\n", pass ? "PASS" : "FAIL", name);
        if (!pass) ok = false;
    };

    sr::Gui gui;
    bool created = gui.Create(hooks, /*hidden=*/true);
    check("window created hidden", created && !gui.IsVisible());
    if (!created) {
        std::printf("guitest: FAIL\n");
        CloseHandle(testQuit);
        return 1;
    }
    // frameless window: taskbar button via WS_EX_APPWINDOW (popup needs it)
    LONG guiEx = GetWindowLongW(gui.Hwnd(), GWL_EXSTYLE);
    check("taskbar button (WS_EX_APPWINDOW, no TOOLWINDOW)",
          (guiEx & WS_EX_APPWINDOW) != 0 && (guiEx & WS_EX_TOOLWINDOW) == 0);

    // wait for webview + page bridge (pump messages manually; no visible window)
    uint64_t t0 = GetTickCount64();
    while (!gui.WebViewReady() && GetTickCount64() - t0 < 10000) {
        gui.PumpMessages();
        Sleep(20);
    }
    check("webview ready", gui.WebViewReady());
    t0 = GetTickCount64();
    while (!gui.PageReady() && GetTickCount64() - t0 < 10000) {
        gui.PumpMessages();
        Sleep(20);
    }
    check("page bridge ready", gui.PageReady());

    // DOM inventory via the bridge
    std::wstring inv;
    bool evalOk = gui.EvalJson(
        L"String('ranges='+document.querySelectorAll('input[type=range]').length"
        L"+';selects='+document.querySelectorAll('select').length"
        L"+';checks='+document.querySelectorAll('input[type=checkbox]').length"
        L"+';buttons='+document.querySelectorAll('button').length)", inv);
    std::printf("  dom inventory: %s\n", sr::ToUtf8(inv).c_str());
    check("dom: 15 sliders, 2 selects, 3 toggles, 9 buttons",
          evalOk && inv.find(L"ranges=15") != std::wstring::npos &&
          inv.find(L"selects=2") != std::wstring::npos &&
          inv.find(L"checks=3") != std::wstring::npos &&
          inv.find(L"buttons=9") != std::wstring::npos);

    // auto-fit: content must fit the frameless window without a scrollbar.
    // The fit timer fires 400 ms after page-ready; pump past it first.
    t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 800) {
        gui.PumpMessages();
        Sleep(20);
    }
    std::wstring fitR;
    bool fitEval = gui.EvalJson(
        L"String('scroll='+document.documentElement.scrollHeight+"
        L"' client='+document.documentElement.clientHeight)", fitR);
    std::printf("  fit measure: %s\n", sr::ToUtf8(fitR).c_str());
    bool fits = false;
    {
        // parse the two numbers; small screens cap the window and scroll
        int sc = 0, cl = 0;
        swscanf(fitR.c_str(), L"\"scroll=%d client=%d\"", &sc, &cl);
        int maxH = GetSystemMetrics(SM_CYSCREEN) - 60;
        if (sc > maxH) {
            std::printf("  note: screen height caps the window (%d), page scrolls with "
                        "the styled thin scrollbar\n", maxH);
            fits = cl > 0; // can't fit; not a failure
        } else {
            fits = sc > 0 && cl > 0 && sc <= cl + 2;
        }
    }
    check("content fits window (no overflow)", fitEval && fits);

    // expected device names for dropdown index 1
    auto caps = sr::EnumerateEndpoints(eCapture);
    auto rens = sr::EnumerateEndpoints(eRender);
    bool comboOk = caps.size() >= 1 && rens.size() >= 1;
    std::wstring expectInput, expectOutput;
    if (comboOk) {
        expectInput = caps.front().name;
        expectOutput = rens.front().name;
        if (sr::IsVirtualAudioName(expectOutput)) {
            size_t v = expectOutput.find(L" (虚拟)");
            if (v != std::wstring::npos) expectOutput.erase(v);
        }
    }

    // inject an apply message as the page would send it
    const wchar_t* applyJson =
        L"{\"cmd\":\"apply\",\"selIn\":1,\"selOut\":1,\"config\":{\"mode\":\"stereo\","
        L"\"overlay_enabled\":true,\"classify_enabled\":true,\"autostart\":false,"
        L"\"fade_ms\":350,\"radar_radius\":120,\"overlay_low\":0.2,\"overlay_high\":0.6,"
        L"\"pos_x_pct\":10,\"pos_y_pct\":25,\"overlay_fx\":80,"
        L"\"weights\":[0.5,1,1,1.2,0.8,0.8,0.9,0.9]}}";
    check("inject apply", gui.InjectBridgeMessage(applyJson));
    t0 = GetTickCount64();
    while (applyCalls == 0 && GetTickCount64() - t0 < 5000) {
        gui.PumpMessages();
        Sleep(10);
    }
    check("apply callback fired once", applyCalls == 1);
    check("device change detected", !comboOk || lastDevChanged);

    // reload config and verify round-trip
    sr::AppConfig c2;
    bool loaded = sr::LoadConfig(configPath, c2);
    check("config file written", loaded);
    std::printf("  config file after Apply:\n");
    {
        FILE* f = nullptr;
        _wfopen_s(&f, configPath.c_str(), L"rb");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) std::printf("  %s", line);
            fclose(f);
        }
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    auto nearf = [](float a, float b) { return std::fabs(a - b) < 0.001f; };
    check("mode = stereo", c2.downmix.mode == sr::DownmixStereo);
    check("fade 350", c2.analysis.fadeMs == 350);
    check("radius 120", c2.overlay.radius == 120);
    check("low 0.20", nearf(c2.overlay.lowThreshold, 0.20f));
    check("high 0.60", nearf(c2.overlay.highThreshold, 0.60f));
    check("fx 80", c2.overlay.fxPct == 80);
    check("posX 10%", c2.overlay.offsetX == 10 * sw / 100);
    check("posY 25%", c2.overlay.offsetY == 25 * sh / 100);
    check("weight FL 0.50", nearf(c2.downmix.weights[0], 0.5f));
    check("weight LFE 1.20", nearf(c2.downmix.weights[3], 1.2f));
    if (comboOk) {
        check("capture_device = dropdown entry 2", c2.captureDevice == expectInput);
        check("output_device = dropdown entry 2", c2.outputDevice == expectOutput);
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
        check("g_downmix mode stereo", sr::g_downmix.cfg.mode == sr::DownmixStereo);
        check("g_downmix weight LFE", nearf(sr::g_downmix.cfg.weights[3], 1.2f));
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        check("g_overlay low 0.20", nearf(sr::g_overlay.cfg.lowThreshold, 0.20f));
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_analysis.mu);
        check("g_analysis fade 350", sr::g_analysis.cfg.fadeMs == 350);
    }

    // locate the Apply button in CSS px now; the real click happens below
    // while the window is visible (WebView2 input needs real input events)
    double btnCx = 0, btnCy = 0;
    {
        std::wstring rectJson;
        bool rectOk = gui.EvalJson(
            L"JSON.stringify(document.getElementById('btnApply').getBoundingClientRect())",
            rectJson);
        auto numAfter = [&](const wchar_t* key) -> double {
            size_t p = rectJson.find(key);
            if (p == std::wstring::npos) return 0;
            return _wtof(rectJson.c_str() + p + wcslen(key));
        };
        double bx = rectOk ? numAfter(L"x\\\":") : 0;
        double by = rectOk ? numAfter(L"y\\\":") : 0;
        double bw = rectOk ? numAfter(L"width\\\":") : 0;
        double bh = rectOk ? numAfter(L"height\\\":") : 0;
        btnCx = bx + bw / 2;
        btnCy = by + bh / 2;
        std::printf("  apply button rect=(%.0f,%.0f %.0fx%.0f)\n", bx, by, bw, bh);
        check("apply button located", rectOk && bw > 0 && bh > 0);
    }

    // exit command -> quit event
    gui.InjectBridgeMessage(L"{\"cmd\":\"exit\"}");
    t0 = GetTickCount64();
    while (WaitForSingleObject(testQuit, 0) != WAIT_OBJECT_0 &&
           GetTickCount64() - t0 < 3000) {
        gui.PumpMessages();
        Sleep(10);
    }
    check("exit command signals quit", WaitForSingleObject(testQuit, 0) == WAIT_OBJECT_0);
    ResetEvent(testQuit);

    // screenshot: the ONE allowed visible window (2 s), WebView2 needs to be
    // visible to render a frame for CapturePreview
    gui.Show();
    gui.EvalJson(L"window.scrollTo(0,0);'ok'", inv); // capture from the top
    t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 2000) {
        gui.PumpMessages();
        Sleep(20);
    }

    // real click on the Apply button through the page's own DOM handler.
    // (Synthetic input is ignored on remote-desktop sessions like ToDesk, and
    // posted WM_LBUTTON* never reaches WebView2's input pipeline; el.click()
    // exercises the same button -> onclick -> bridge -> C++ path.)
    {
        int clicksBefore = applyCalls;
        std::wstring clickRes;
        bool clickEval = gui.EvalJson(
            L"document.getElementById('btnApply').click();'clicked'", clickRes);
        uint64_t t1 = GetTickCount64();
        while (applyCalls == clicksBefore && GetTickCount64() - t1 < 3000) {
            gui.PumpMessages();
            Sleep(10);
        }
        std::printf("  dom click on 应用 Apply: eval=%s\n", clickEval ? "ok" : "FAIL");
        check("DOM click on 应用 Apply fires apply", applyCalls == clicksBefore + 1);
    }

    bool shotOk = gui.CapturePng(L"build\\gui-modern.png");
    gui.Hide();
    std::printf("  gui screenshot: build\\gui-modern.png (%s)\n", shotOk ? "written" : "FAILED");
    check("gui-modern.png captured", shotOk);

    DestroyWindow(gui.Hwnd());
    CloseHandle(testQuit);
    DeleteFileW(configPath.c_str());
    std::printf("guitest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int RunSimulateGui(sr::AppConfig cfg) {
    using namespace sr; // control IDs
    sr::ComInit com;
    if (!com.Ok()) return 1;
    cfg.overlay.enabled = true;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    {
        std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
        sr::g_downmix.cfg = cfg.downmix;
    }

    sr::SharedMeters meters;
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/false); // headless test
    sr::Simulator sim(sr::SimDual, &meters, g_quit);
    sim.Start();

    std::wstring configPath = TempConfigPath(L"soundradar_simgui_config.json");
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.onApply = [](bool) {};
    hooks.statusLevel = [] { return 0; };
    hooks.statusText = [] { return std::wstring(L"simulate-gui"); };
    sr::Gui gui;
    bool guiOk = gui.Create(hooks, /*hidden=*/true);

    HWND overlayHwnd = WaitForOverlayWindow(overlay, 3000);
    // wait for the JS bridge, then apply via an injected bridge message
    uint64_t t0 = GetTickCount64();
    while (!gui.PageReady() && GetTickCount64() - t0 < 10000) {
        gui.PumpMessages();
        Sleep(20);
    }
    Sleep(800);

    gui.InjectBridgeMessage(
        L"{\"cmd\":\"apply\",\"selIn\":0,\"selOut\":0,\"config\":{\"overlay_low\":0.30,"
        L"\"overlay_fx\":90,\"weights\":[0.7,1.25,1,0.7,0.8,0.8,0.9,0.9]}}");
    t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 3000) { // let the message land
        gui.PumpMessages();
        Sleep(10);
        float lw;
        {
            std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
            lw = sr::g_overlay.cfg.lowThreshold;
        }
        if (std::fabs(lw - 0.30f) < 0.001f) break;
    }

    // wait for the overlay thread to apply the new config version
    bool overlayApplied = false;
    for (int i = 0; i < 40 && !overlayApplied; ++i) {
        uint32_t want;
        {
            std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
            want = sr::g_overlay.version;
        }
        overlayApplied = overlay.AppliedVersion() == want;
        if (!overlayApplied) Sleep(50);
    }
    float lowNow;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        lowNow = sr::g_overlay.cfg.lowThreshold;
    }
    float frWeight;
    {
        std::lock_guard<std::mutex> lk(sr::g_downmix.mu);
        frWeight = sr::g_downmix.cfg.weights[1];
    }

    Sleep(1500); // let the overlay draw with the new config
    uint64_t frames = overlay.FramesDrawn();

    sim.Stop();
    overlay.Stop();
    if (gui.Hwnd()) DestroyWindow(gui.Hwnd());
    DeleteFileW(configPath.c_str());

    bool ok = guiOk && overlayHwnd && overlayApplied && frames > 0 &&
              std::fabs(lowNow - 0.30f) < 0.001f && std::fabs(frWeight - 1.25f) < 0.001f;
    std::printf("simulate-gui: gui=%s overlayWindow=%s overlayApplied=%s "
                "frames=%llu lowThreshold=%.2f frWeight=%.2f -> %s\n",
                guiOk ? "ok" : "FAIL", overlayHwnd ? "ok" : "FAIL",
                overlayApplied ? "ok" : "FAIL", (unsigned long long)frames, lowNow,
                frWeight, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// --- on-screen proof: visible overlay + CopyFromScreen pixel check -----------

// Captures a screen region (absolute coords) via GDI and saves as 24-bit BMP.
bool CaptureScreenRegion(int x, int y, int w, int h, const std::wstring& path,
                         std::vector<uint8_t>& rgbOut) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h; // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    rgbOut.resize(static_cast<size_t>(w) * h * 3);
    GetDIBits(mem, bmp, 0, h, rgbOut.data(), reinterpret_cast<BITMAPINFO*>(&bi),
              DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    uint32_t imgSize = static_cast<uint32_t>(rgbOut.size());
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfSize = sizeof(fh) + sizeof(bi) + imgSize;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    BITMAPINFOHEADER bih = bi;
    bih.biHeight = h; // bottom-up for file readers
    // rows are already top-down in rgbOut; flip while writing
    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&bih, 1, sizeof(bih), f);
    size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3);
    std::vector<uint8_t> row(stride);
    for (int r = h - 1; r >= 0; --r) {
        std::memcpy(row.data(), rgbOut.data() + static_cast<size_t>(r) * w * 3,
                    static_cast<size_t>(w) * 3);
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

int RunOnscreenProof(sr::AppConfig cfg) {
    cfg.overlay.enabled = true;
    // park the proof radar left of center so a concurrently running production
    // instance's overlay can't overlap our pixel assertions
    cfg.overlay.offsetX = -GetSystemMetrics(SM_CXSCREEN) / 4;
    cfg.overlay.offsetY = 0;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    sr::SharedMeters meters;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int cx = sw / 2 + cfg.overlay.offsetX;
    int cy = sh / 2 + cfg.overlay.offsetY;
    int r = cfg.overlay.radius;
    int box = r + 70;
    int W = box * 2;

    bool capOk = false, arrowFL = false, arrowBR = false;
    double centerDiff = 1e9;
    int diffPixels = 0;
    int lastBandDiff = 0;

    // retry: the user's desktop may change between the two captures
    for (int attempt = 0; attempt < 3; ++attempt) {
        sr::Overlay overlay;
        overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/true);

        // deterministic meter feed: FL+BR arrows (red). Nothing else is drawn
        // near the center - the proof asserts the center stays untouched.
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            for (int c = 0; c < 8; ++c) {
                meters.frame.level[c] = 0.0f;
                meters.frame.peak[c] = false;
            }
            meters.frame.level[0] = 0.8f; // FL
            meters.frame.level[5] = 0.8f; // BR
            meters.frame.peak[0] = meters.frame.peak[5] = true;
            meters.frame.active = true;
        }

        HWND hwnd = WaitForOverlayWindow(overlay, 3000);
        Sleep(2500); // let it compose a few frames

        std::vector<uint8_t> rgb;
        capOk = CaptureScreenRegion(cx - box, cy - box, W, W,
                                    L"build\\onscreen-proof.bmp", rgb);
        // edge capsules: FL projects onto the top border, BR onto the bottom.
        // Capture both strips (positions derived from the same projection).
        std::vector<uint8_t> bandTopOn, bandBottomOn;
        CaptureScreenRegion(100, 0, 300, 40, L"build\\proof-band-top.bmp", bandTopOn);
        CaptureScreenRegion(1250, sh - 40, 220, 40, L"build\\proof-band-bottom.bmp",
                            bandBottomOn);

        // background-independent diff: capture again with the overlay gone
        overlay.Stop();
        Sleep(400);
        std::vector<uint8_t> rgbOff;
        CaptureScreenRegion(cx - box, cy - box, W, W, L"build\\onscreen-proof-off.bmp",
                            rgbOff);
        std::vector<uint8_t> bandTopOff, bandBottomOff;
        CaptureScreenRegion(100, 0, 300, 40, L"build\\proof-band-top-off.bmp", bandTopOff);
        CaptureScreenRegion(1250, sh - 40, 220, 40, L"build\\proof-band-bottom-off.bmp",
                            bandBottomOff);

        auto lum = [&](const std::vector<uint8_t>& buf, int px, int py) -> double {
            size_t i = (static_cast<size_t>(py) * W + px) * 3;
            return 0.114 * buf[i] + 0.587 * buf[i + 1] + 0.299 * buf[i + 2];
        };
        int lx = box, ly = box; // region-local center

        // 1) arrows present at the right angles: FL (-30 deg) and BR (135 deg).
        //    Pick the most RED-SATURATED pixel in a 7x7 box (max luminance
        //    would pick washed-out glow over a bright background).
        auto reddish = [&](double deg) {
            double a = deg * 3.14159265358979 / 180.0;
            int px = lx + static_cast<int>(std::sin(a) * r); // arc band sits on r
            int py = ly - static_cast<int>(std::cos(a) * r);
            double bestSat = -1e9;
            int br = 0, bg = 0, bb = 0;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx2 = -3; dx2 <= 3; ++dx2) {
                    size_t i = (static_cast<size_t>(py + dy) * W + px + dx2) * 3;
                    int B = rgb[i], G = rgb[i + 1], R = rgb[i + 2];
                    double sat = R - (G + B) / 2.0;
                    if (sat > bestSat) {
                        bestSat = sat;
                        br = R; bg = G; bb = B;
                    }
                }
            return bestSat > 40.0 && br > 150;
        };
        arrowFL = reddish(-30.0);
        arrowBR = reddish(135.0);

        // 2) center stays 100% see-through: center 20x20 must match the
        //    overlay-off capture almost exactly
        centerDiff = 0;
        for (int py = ly - 10; py < ly + 10; ++py)
            for (int px = lx - 10; px < lx + 10; ++px)
                centerDiff += std::fabs(lum(rgb, px, py) - lum(rgbOff, px, py));
        centerDiff /= 400.0;

        // 3) overlay-gone diff: changed pixels between the two captures
        diffPixels = 0;
        for (int py = 0; py < W; ++py)
            for (int px = 0; px < W; ++px) {
                double d = std::fabs(lum(rgb, px, py) - lum(rgbOff, px, py));
                if (d > 10.0) ++diffPixels;
            }
        // 4) edge capsules: top strip (FL) and bottom strip (BR) must change
        int bandDiff = 0;
        auto bandDelta = [&](const std::vector<uint8_t>& on,
                             const std::vector<uint8_t>& off) {
            int n = 0;
            for (size_t i = 0; i + 2 < on.size() && i + 2 < off.size(); i += 3) {
                double d = 0.0;
                for (int k = 0; k < 3; ++k)
                    d += std::fabs(static_cast<int>(on[i + k]) - off[i + k]);
                if (d > 30.0) ++n;
            }
            return n;
        };
        bandDiff = bandDelta(bandTopOn, bandTopOff) + bandDelta(bandBottomOn, bandBottomOff);
        lastBandDiff = bandDiff;

        if (attempt > 0)
            std::printf("  (retry %d: desktop changed between captures)\n", attempt);
        if (capOk && arrowFL && arrowBR && centerDiff < 2.0 && diffPixels > 200 &&
            bandDiff > 30) {
            std::printf("  (overlay hwnd %s, %llu frames drawn)\n",
                        hwnd ? "ok" : "MISSING",
                        (unsigned long long)overlay.FramesDrawn());
            break;
        }
    }

    bool ok = capOk && arrowFL && arrowBR && centerDiff < 2.0 && diffPixels > 200 &&
              lastBandDiff > 30;
    std::printf("onscreen-proof: capture=%s arrows FL=%s BR=%s centerDiff=%.2f "
                "diffPixels=%d bandPixels=%d -> %s\n",
                capOk ? "ok" : "FAIL", arrowFL ? "red" : "NO", arrowBR ? "red" : "NO",
                centerDiff, diffPixels, lastBandDiff, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// --- style shot: visible overlay with a crafted scene, captured to BMP ------

int RunStyleShot(sr::AppConfig cfg) {
    cfg.overlay.enabled = true;
    cfg.overlay.offsetX = 0;
    cfg.overlay.offsetY = 0;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    sr::SharedMeters meters;
    {
        std::lock_guard<std::mutex> lk(meters.mu);
        for (int c = 0; c < 8; ++c) {
            meters.frame.level[c] = 0.0f;
            meters.frame.peak[c] = false;
            meters.classes[c] = sr::SoundNone;
        }
        meters.frame.level[0] = 0.55f; // FL: footstep, amber
        meters.frame.level[5] = 0.95f; // BR: gunshot, red-magenta
        meters.frame.level[7] = 0.35f; // SR: weak, cyan
        meters.frame.peak[5] = true;
        meters.frame.active = true;
        meters.classes[0] = sr::SoundFootstep;
        meters.classes[5] = sr::SoundGunshot;
    }
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/true);
    HWND hwnd = WaitForOverlayWindow(overlay, 3000);
    Sleep(2500);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int cx = sw / 2, cy = sh / 2;
    int box = cfg.overlay.radius + 110;
    std::vector<uint8_t> rgb;
    // radar crop for detail review
    bool capOk = CaptureScreenRegion(cx - box, cy - box, box * 2, box * 2,
                                     L"build\\overlay-style.bmp", rgb);
    // full screen: edge capsules are at the borders
    std::vector<uint8_t> full;
    CaptureScreenRegion(0, 0, sw, sh, L"build\\overlay-style-full.bmp", full);
    std::printf("style-shot: capture=%s hwnd=%s frames=%llu -> build\\overlay-style.bmp\n",
                capOk ? "ok" : "FAIL", hwnd ? "ok" : "MISSING",
                (unsigned long long)overlay.FramesDrawn());
    overlay.Stop();
    return capOk ? 0 : 1;
}

// --- orbit tests: moving direction arrows ------------------------------------

namespace {

float ShortestAngDist(float a, float b) {
    return std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
}

// cosine-power panning law: level of a channel at chDeg for a source at srcDeg
float PanLevel(float srcDeg, float chDeg, float amp) {
    float d = std::fabs(ShortestAngDist(srcDeg, chDeg));
    if (d >= 90.0f) return 0.0f;
    float c = std::cos(d * 3.14159265f / 180.0f);
    return amp * c * c;
}

const float kChAngle[8] = { -30, 30, 0, -999, -135, 135, -90, 90 };


// --- pan sweep: stereo pan tracking -------------------------------------------

// --simulate pan-sweep: srcChannels=2, pan sweeps -1..+1 sinusoidally; the
// single indicator must track angle = pan * 90 deg within 12 deg mean error.
int RunPanSweep(sr::AppConfig cfg) {
    cfg.overlay.enabled = true;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    sr::SharedMeters meters;
    {
        std::lock_guard<std::mutex> lk(meters.mu);
        meters.srcChannels = 2;
    }
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/false);
    HWND hwnd = WaitForOverlayWindow(overlay, 3000);
    (void)hwnd;

    const double totalT = 6.0;
    double errSum = 0;
    int errN = 0;
    int lastPrint = -1;
    for (int i = 0; i < 300; ++i) {
        double t = i * 0.02;
        float pan = std::sin(static_cast<float>(2.0 * 3.14159265 * t / totalT));
        float lvlL = 0.35f * (1.0f - pan);
        float lvlR = 0.35f * (1.0f + pan);
        sr::AnalysisFrame fr{};
        fr.level[0] = lvlL;
        fr.level[1] = lvlR;
        fr.active = true;
        fr.peak[0] = lvlL >= 0.5f;
        fr.peak[1] = lvlR >= 0.5f;
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            meters.frame = fr;
        }
        Sleep(20);

        if (i / 25 != lastPrint && i > 25) {
            lastPrint = i / 25;
            float expected = pan * 90.0f;
            std::vector<float> arrows = overlay.DebugArrowAngles();
            float bestErr = 180.0f, bestA = 0;
            for (float a : arrows) {
                float d = std::fabs(ShortestAngDist(a, expected));
                if (d < bestErr) { bestErr = d; bestA = a; }
            }
            std::printf("t=%4.1fs pan=%+.2f expected=%+5.1f measured=%+5.1f err=%4.1f\n",
                        t, pan, expected, bestA, bestErr);
            errSum += bestErr;
            ++errN;
        }
    }
    overlay.Stop();
    double meanErr = errN ? errSum / errN : 180.0;
    bool ok = meanErr < 12.0 && errN > 0;
    std::printf("pan-sweep: mean abs error %.1f deg over %d samples -> %s\n", meanErr,
                errN, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
} // namespace

// --simulate orbit: one source rotates 360 deg over ~8 s; the tracked arrow
// must follow. --simulate dual-orbit: fixed FL source + orbiting source.
int RunOrbitTest(sr::AppConfig cfg, bool dual) {
    cfg.overlay.enabled = true;
    {
        std::lock_guard<std::mutex> lk(sr::g_overlay.mu);
        sr::g_overlay.cfg = cfg.overlay;
        ++sr::g_overlay.version;
    }
    sr::SharedMeters meters;
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit, /*visible=*/false); // headless
    HWND hwnd = WaitForOverlayWindow(overlay, 3000);

    const double totalT = 8.0; // one revolution
    double errSum = 0, fixedErrSum = 0;
    int errN = 0, fixedN = 0;
    int lastPrint = -1;

    for (int i = 0; i < 400; ++i) {
        double t = i * 0.02;
        float theta = std::fmod(static_cast<float>(-180.0 + 360.0 * t / totalT) + 540.0f,
                                360.0f) - 180.0f;
        sr::AnalysisFrame fr{};
        for (int c = 0; c < 8; ++c) {
            if (c == 3) continue; // LFE
            float l = PanLevel(theta, kChAngle[c], 0.85f);
            if (dual) {
                float f = PanLevel(-30.0f, kChAngle[c], 0.8f);
                if (f > l) l = f;
            }
            fr.level[c] = l;
            fr.peak[c] = l >= 0.5f;
            if (l >= 0.05f) fr.active = true;
        }
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            meters.frame = fr;
        }
        Sleep(20);

        if (i / 25 != lastPrint && i > 25) { // every 500 ms after settle
            lastPrint = i / 25;
            std::vector<float> arrows = overlay.DebugArrowAngles();
            if (dual) {
                // fixed arrow: nearest to -30; moving arrow: nearest to theta
                float bestFixed = 1e9f, bestMove = 1e9f, fixedA = 0, moveA = 0;
                for (float a : arrows) {
                    float df = std::fabs(ShortestAngDist(a, -30.0f));
                    float dm = std::fabs(ShortestAngDist(a, theta));
                    if (df < bestFixed) { bestFixed = df; fixedA = a; }
                    if (dm < bestMove) { bestMove = dm; moveA = a; }
                }
                std::printf("t=%4.1fs orbit=%5.1f arrows: fixed=%5.1f (err %4.1f) "
                            "moving=%5.1f (err %4.1f)\n",
                            t, theta, fixedA, bestFixed, moveA, bestMove);
                if (arrows.size() >= 2) {
                    fixedErrSum += bestFixed;
                    ++fixedN;
                }
                errSum += bestMove;
                ++errN;
            } else {
                float bestErr = 180.0f, bestA = 0;
                for (float a : arrows) {
                    float d = std::fabs(ShortestAngDist(a, theta));
                    if (d < bestErr) { bestErr = d; bestA = a; }
                }
                std::printf("t=%4.1fs expected=%5.1f measured=%5.1f err=%4.1f\n", t,
                            theta, bestA, bestErr);
                errSum += bestErr;
                ++errN;
            }
        }
    }

    overlay.Stop();
    (void)hwnd;
    double meanErr = errN ? errSum / errN : 180.0;
    double meanFixed = fixedN ? fixedErrSum / fixedN : (dual ? 180.0 : 0.0);
    bool ok;
    if (dual) {
        ok = meanErr < 15.0 && meanFixed < 10.0 && errN > 0 && fixedN > 0;
        std::printf("dual-orbit: mean moving err %.1f deg, mean fixed err %.1f deg -> %s\n",
                    meanErr, meanFixed, ok ? "PASS" : "FAIL");
    } else {
        ok = meanErr < 15.0 && errN > 0;
        std::printf("orbit: mean abs error %.1f deg over %d samples -> %s\n", meanErr, errN,
                    ok ? "PASS" : "FAIL");
    }
    return ok ? 0 : 1;
}

} // namespace

// --diag: listen-only health check, 10 s, plays NOTHING (ear-safe).
// Prints per-channel peak/RMS so we can see whether game audio reaches us.
int RunDiag(sr::AppConfig& cfg) {
    sr::ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }
    sr::CaptureClient cap;
    std::wstring err;
    if (!cap.Init(cfg.captureDevice, err)) {
        std::fprintf(stderr, "%s\n", sr::ToUtf8(err).c_str());
        return 1;
    }
    const sr::CaptureClient::Format& f = cap.GetFormat();
    std::printf("capture: %s (%u Hz, %u ch, buffer %.1f ms)\n",
                sr::ToUtf8(cap.DeviceName()).c_str(), f.sampleRate, f.channels,
                f.bufferFrames * 1000.0 / f.sampleRate);
    cap.ProbeChannelCounts();
    std::printf("listening 10 s (no audio output). Play a video or the game now...\n");

    float maxLv[8] = {};
    double sumSq[8] = {};
    unsigned long long totalFrames = 0, packets = 0;
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread t([&] {
        cap.Run([&](const float* frames, uint32_t n, LONGLONG) {
            ++packets;
            totalFrames += n;
            for (uint32_t i = 0; i < n; ++i)
                for (int c = 0; c < 8; ++c) {
                    float v = std::fabs(frames[static_cast<size_t>(i) * 8 + c]);
                    if (v > maxLv[c]) maxLv[c] = v;
                    sumSq[c] += static_cast<double>(v) * v;
                }
        }, quit);
    });
    Sleep(10000);
    SetEvent(quit);
    t.join();
    CloseHandle(quit);

    static const char* kNames[8] = { "FL", "FR", "C", "LFE", "BL", "BR", "SL", "SR" };
    std::printf("\n%-5s %8s %8s\n", "ch", "max", "rms");
    bool any = false;
    for (int c = 0; c < 8; ++c) {
        double rms = totalFrames ? std::sqrt(sumSq[c] / totalFrames) : 0.0;
        std::printf("%-5s %8.3f %8.3f\n", kNames[c], maxLv[c], rms);
        if (maxLv[c] > 0.02f) any = true;
    }
    std::printf("packets=%llu frames=%llu\n", packets, totalFrames);
    std::printf(any ? "RESULT: audio reaches the engine; channels above are live\n"
                    : "RESULT: silence. Check: Potato strip 'Voicemeeter Input' has B1 ON; game output = Voicemeeter Input; some audio playing\n");
    return any ? 0 : 2;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    sr::LogInit();

    std::wstring configPath = sr::DefaultConfigPath();
    std::wstring outputOverride;
    std::wstring modeOverride;
    std::wstring screenshotPath;
    std::wstring simScenario;
    std::wstring overlayTestShot;
    bool selftest = false, measure = false, measureLoopback = false, list = false;
    bool trayMode = false, overlayTest = false, classifyTest = false;
    bool guiTest = false, simGui = false, diag = false, onscreenProof = false;
    bool setDefault = false;
    bool styleShot = false;
    std::wstring setDefaultNeedle; // optional explicit render-device substring
    int panTestSeconds = -1;

    std::wstring argLine;
    for (int i = 1; i < argc; ++i) {
        argLine += argv[i];
        argLine += L' ';
    }
    sr::Log("startup: args=[%s]", sr::ToUtf8(argLine).c_str());

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto needValue = [&](const wchar_t* flag) -> std::wstring {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"%ls requires a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == L"--selftest") selftest = true;
        else if (a == L"--classifytest") classifyTest = true;
        else if (a == L"--guitest") guiTest = true;
        else if (a == L"--simulate-gui") simGui = true;
        else if (a == L"--onscreen-proof") onscreenProof = true;
        else if (a == L"--style-shot") styleShot = true;
        else if (a == L"--measure") measure = true;
        else if (a == L"--measure-loopback") measureLoopback = true;
        else if (a == L"--pan-test") {
            panTestSeconds = 0;
            if (i + 1 < argc && argv[i + 1][0] != L'-')
                panTestSeconds = _wtoi(argv[++i]);
        }
        else if (a == L"--set-default") {
            setDefault = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') setDefaultNeedle = argv[++i];
        }
        else if (a == L"--list-devices") list = true;
        else if (a == L"--diag") diag = true;
        else if (a == L"--tray") trayMode = true;
        else if (a == L"--overlaytest") {
            overlayTest = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') overlayTestShot = argv[++i];
        }
        else if (a == L"--simulate") simScenario = needValue(L"--simulate");
        else if (a == L"--simulate-screenshot") screenshotPath = needValue(L"--simulate-screenshot");
        else if (a == L"--output") outputOverride = needValue(L"--output");
        else if (a == L"--mode") modeOverride = needValue(L"--mode");
        else if (a == L"--config") configPath = needValue(L"--config");
        else if (a == L"--help" || a == L"-h" || a == L"/?") {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", sr::ToUtf8(a).c_str());
            PrintUsage();
            return 1;
        }
    }

    if (selftest) return sr::RunSelfTest();
    if (classifyTest) return sr::RunClassifyTest();

    sr::AppConfig cfg;
    LoadConfig(configPath, cfg); // missing file = defaults

    if (list) {
        ListDevices(cfg.captureDevice);
        return 0;
    }
    if (!outputOverride.empty()) cfg.outputDevice = outputOverride;
    if (!modeOverride.empty()) {
        if (modeOverride == L"right-mono") cfg.downmix.mode = sr::DownmixRightMono;
        else if (modeOverride == L"stereo") cfg.downmix.mode = sr::DownmixStereo;
        else {
            fwprintf(stderr, L"--mode must be right-mono or stereo\n");
            return 1;
        }
    }
    if (measure) return sr::RunMeasure(cfg.outputDevice, cfg.captureDevice);
    if (measureLoopback) return sr::RunMeasureLoopback();
    if (panTestSeconds >= 0) return sr::RunPanTest(panTestSeconds);
    if (setDefault) {
        sr::ComInit com;
        std::printf("default render before: %s\n", sr::ToUtf8(sr::DefaultRenderName()).c_str());
        std::wstring needle = setDefaultNeedle;
        if (needle.empty()) {
            auto cands = sr::SelectCaptureEndpoints(cfg.captureDevice);
            if (cands.empty()) {
                std::printf("no capture endpoint matches the capture_device rule\n");
                return 1;
            }
            std::printf("capture selected : %s\n", sr::ToUtf8(cands.front().name).c_str());
            needle = sr::RenderCounterpartNeedle(cands.front().name);
        }
        std::printf("setting default  : render device matching \"%s\"\n",
                    sr::ToUtf8(needle).c_str());
        if (needle.empty() || !sr::SetDefaultRenderDevice(needle)) {
            std::printf("FAILED (see %%APPDATA%%\\SoundRadar\\log.txt)\n");
            return 1;
        }
        std::printf("default render after : %s\n", sr::ToUtf8(sr::DefaultRenderName()).c_str());
        return 0;
    }
    if (diag) return RunDiag(cfg);

    if (trayMode || argc == 1) FreeConsole(); // autostart or Explorer double-click: no console window

    // first run: write a default config so users have something to edit
    {
        wchar_t full[MAX_PATH] = {};
        DWORD n = GetFullPathNameW(configPath.c_str(), MAX_PATH, full, nullptr);
        std::wstring p = (n > 0 && n < MAX_PATH) ? full : configPath;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (sr::SaveConfig(p, cfg))
                if (GetConsoleWindow())
                    std::printf("wrote default config: %s\n", sr::ToUtf8(p).c_str());
        }
    }

    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_quit) {
        fwprintf(stderr, L"CreateEvent failed\n");
        return 1;
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    int rc = 0;
    if (!screenshotPath.empty()) {
        float dualLevels[8] = {};
        dualLevels[0] = 0.8f;
        dualLevels[5] = 0.8f;
        uint8_t cls[8] = {};
        cls[0] = sr::SoundFootstep;
        cls[5] = sr::SoundGunshot;
        int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        bool ok = sr::RenderSceneToFile(screenshotPath, w, h, dualLevels, cls, cfg.overlay);
        std::printf("screenshot %s: %s\n", ok ? "written" : "FAILED",
                    sr::ToUtf8(screenshotPath).c_str());
        rc = ok ? 0 : 1;
    } else if (overlayTest) {
        rc = RunOverlayTest(cfg, overlayTestShot.empty() ? L"overlay-dual.bmp" : overlayTestShot);
    } else if (guiTest) {
        rc = RunGuiTest();
    } else if (onscreenProof) {
        rc = RunOnscreenProof(cfg); // the ONE allowed visible-window test
    } else if (styleShot) {
        rc = RunStyleShot(cfg); // visible art-direction capture
    } else if (simGui) {
        rc = RunSimulateGui(cfg);
    } else if (!simScenario.empty()) {
        if (simScenario == L"orbit") {
            rc = RunOrbitTest(cfg, false);
        } else if (simScenario == L"dual-orbit") {
            rc = RunOrbitTest(cfg, true);
        } else if (simScenario == L"pan-sweep") {
            rc = RunPanSweep(cfg);
        } else {
            sr::SimScenario sc = sr::SimSweep;
            if (simScenario == L"sweep") sc = sr::SimSweep;
            else if (simScenario == L"dual") sc = sr::SimDual;
            else if (simScenario == L"pulse") sc = sr::SimPulse;
            else {
                fwprintf(stderr, L"--simulate must be sweep, dual, pulse, orbit or dual-orbit\n");
                rc = 1;
            }
            if (rc == 0) rc = RunSimulate(cfg, sc);
        }
    } else {
        // GUI / tray mode: single instance enforced here (test modes bypass it)
        if (!EnsureSingleInstance()) {
            rc = 0;
        } else {
            rc = RunApp(cfg, configPath, trayMode, /*showGui=*/!trayMode);
        }
    }

    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    CloseHandle(g_quit);
    g_quit = nullptr;
    if (g_singleton) {
        CloseHandle(g_singleton);
        g_singleton = nullptr;
    }
    return rc;
}
