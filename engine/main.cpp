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
#include "downmix.h"
#include "gui.h"
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
        "  --simulate <scenario>  sweep | dual | pulse; feeds synthetic meters ~12 s\n"
        "  --simulate-screenshot <file.bmp>  render one dual frame to a BMP\n"
        "  --overlaytest [bmp]    overlay checks: ex-style, CPU, screenshots\n"
        "  --measure              latency report (needs SoundRadar driver + output)\n"
        "  --measure-loopback     click-train round-trip through the SoundRadar driver\n"
        "  --pan-test [seconds]   play per-channel test tones on the SoundRadar speaker\n"
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
        if (!ren_->Init(cfg.outputDevice, lastError)) {
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
        sr::Log("pipeline: capture=%s render=%s (%s)",
                sr::ToUtf8(capName).c_str(), sr::ToUtf8(renName).c_str(),
                exclusive ? "exclusive" : "shared");

        ring_ = std::make_unique<sr::RingBuffer>(8192);
        analyzer_ = std::make_unique<sr::Analyzer>(cfg.analysis);
        classifier_ = std::make_unique<sr::Classifier8>();
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

private:
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
        overlay.Start(cfg.overlay, &meters, g_quit);
        sr::Log("overlay: started (enabled)");
    }

    sr::Tray tray;
    sr::Gui gui;
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.statusText = [&]() -> std::wstring {
        if (!pipeline.running)
            return L"等待设备 Waiting for device\r\n" + pipeline.lastError;
        wchar_t buf[1024];
        double latency = pipeline.capBufMs + pipeline.periodMs + pipeline.renBufMs;
        swprintf_s(buf, L"运行中 Running\r\n输入 In: %s\r\n输出 Out: %s (%s)\r\n估计延迟 Latency ~%.0f ms",
                   pipeline.capName.c_str(), pipeline.renName.c_str(),
                   pipeline.exclusive ? L"独占 exclusive" : L"共享 shared", latency);
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
        if (on) overlay.Start(cfg.overlay, &meters, g_quit);
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
    overlay.Start(cfg.overlay, &meters, g_quit);

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
    overlay.Start(cfg.overlay, &meters, g_quit);

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

int RunGuiTest() {
    using namespace sr; // control IDs
    sr::ComInit com;
    if (!com.Ok()) return 1;

    sr::AppConfig cfg; // defaults
    std::wstring configPath = TempConfigPath(L"soundradar_guitest_config.json");
    DeleteFileW(configPath.c_str());

    int applyCalls = 0;
    bool lastDevChanged = false;
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.onApply = [&](bool devChanged) {
        ++applyCalls;
        lastDevChanged = devChanged;
    };
    hooks.statusText = [] { return std::wstring(L"test status"); };

    sr::Gui gui;
    if (!gui.Create(hooks, /*hidden=*/true)) {
        std::printf("[FAIL] gui.Create failed\n");
        return 1;
    }
    HWND hwnd = gui.Hwnd();
    std::printf("gui created hidden: visible=%d (must be 0)\n", IsWindowVisible(hwnd) ? 1 : 0);

    // control inventory
    std::printf("control inventory:\n");
    struct EnumCtx { int count; };
    EnumCtx ectx{ 0 };
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM lp) -> BOOL {
            wchar_t cls[64] = {}, text[128] = {};
            GetClassNameW(child, cls, 64);
            GetWindowTextW(child, text, 128);
            int id = GetDlgCtrlID(child);
            std::printf("  id=%5d  %-16s  %s\n", id, sr::ToUtf8(cls).c_str(),
                        sr::ToUtf8(text).c_str());
            ++reinterpret_cast<EnumCtx*>(lp)->count;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ectx));
    std::printf("  total controls: %d\n", ectx.count);

    // manipulate: 2nd entry of each device dropdown, stereo radio, sliders
    auto send = [&](int id, UINT msg, WPARAM wp, LPARAM lp) {
        return SendMessageW(GetDlgItem(hwnd, id), msg, wp, lp);
    };
    int inCount = (int)send(IDC_COMBO_INPUT, CB_GETCOUNT, 0, 0);
    int outCount = (int)send(IDC_COMBO_OUTPUT, CB_GETCOUNT, 0, 0);
    bool comboOk = inCount >= 2 && outCount >= 2;
    std::wstring expectInput, expectOutput;
    if (comboOk) {
        send(IDC_COMBO_INPUT, CB_SETCURSEL, 1, 0);
        send(IDC_COMBO_OUTPUT, CB_SETCURSEL, 1, 0);
        wchar_t buf[256] = {};
        send(IDC_COMBO_INPUT, CB_GETLBTEXT, 1, (LPARAM)buf);
        expectInput = buf;
        wchar_t buf2[256] = {};
        send(IDC_COMBO_OUTPUT, CB_GETLBTEXT, 1, (LPARAM)buf2);
        expectOutput = buf2;
        size_t v = expectOutput.find(L" (虚拟)");
        if (v != std::wstring::npos) expectOutput.erase(v);
    } else {
        std::printf("  note: fewer than 2 endpoints, skipping dropdown selection\n");
    }
    CheckRadioButton(hwnd, IDC_RADIO_MONO, IDC_RADIO_STEREO, IDC_RADIO_STEREO);
    auto setS = [&](int id, int v) { send(id, TBM_SETPOS, TRUE, v); };
    setS(IDC_SLIDER_FADE, 350);
    setS(IDC_SLIDER_RADIUS, 120);
    setS(IDC_SLIDER_LOW, 20);
    setS(IDC_SLIDER_HIGH, 60);
    setS(IDC_SLIDER_POSX, 10);
    setS(IDC_SLIDER_POSY, 25);
    setS(IDC_SLIDER_FX, 80);
    setS(IDC_SLIDER_W0 + 0, 10); // FL 0.50
    setS(IDC_SLIDER_W0 + 3, 24); // LFE 1.20

    send(IDC_BTN_APPLY, BM_CLICK, 0, 0);

    // reload config and verify round-trip
    sr::AppConfig c2;
    bool loaded = sr::LoadConfig(configPath, c2);
    std::printf("config file after Apply:\n");
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
    bool ok = loaded;
    auto check = [&](const char* name, bool pass) {
        std::printf("  [%s] %s\n", pass ? "PASS" : "FAIL", name);
        if (!pass) ok = false;
    };
    check("apply callback fired once", applyCalls == 1);
    check("device change detected", !comboOk || lastDevChanged);
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
    // live globals updated
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

    DestroyWindow(hwnd);
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
    overlay.Start(cfg.overlay, &meters, g_quit);
    sr::Simulator sim(sr::SimDual, &meters, g_quit);
    sim.Start();

    std::wstring configPath = TempConfigPath(L"soundradar_simgui_config.json");
    sr::Gui::Hooks hooks;
    hooks.cfg = &cfg;
    hooks.configPath = configPath;
    hooks.onApply = [](bool) {};
    hooks.statusText = [] { return std::wstring(L"simulate-gui"); };
    sr::Gui gui;
    bool guiOk = gui.Create(hooks, /*hidden=*/true);

    HWND overlayHwnd = WaitForOverlayWindow(overlay, 3000);
    Sleep(800);

    // change thresholds + a weight through the GUI, then Apply
    auto send = [&](int id, UINT msg, WPARAM wp, LPARAM lp) {
        return SendMessageW(GetDlgItem(gui.Hwnd(), id), msg, wp, lp);
    };
    send(IDC_SLIDER_LOW, TBM_SETPOS, TRUE, 30);  // 0.30
    send(IDC_SLIDER_FX, TBM_SETPOS, TRUE, 90);
    send(IDC_SLIDER_W0 + 1, TBM_SETPOS, TRUE, 25); // FR 1.25
    send(IDC_BTN_APPLY, BM_CLICK, 0, 0);

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

} // namespace

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
    bool guiTest = false, simGui = false;
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
        else if (a == L"--measure") measure = true;
        else if (a == L"--measure-loopback") measureLoopback = true;
        else if (a == L"--pan-test") {
            panTestSeconds = 0;
            if (i + 1 < argc && argv[i + 1][0] != L'-')
                panTestSeconds = _wtoi(argv[++i]);
        }
        else if (a == L"--list-devices") list = true;
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

    if (trayMode) FreeConsole(); // autostart: no console window

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
    } else if (simGui) {
        rc = RunSimulateGui(cfg);
    } else if (!simScenario.empty()) {
        sr::SimScenario sc = sr::SimSweep;
        if (simScenario == L"sweep") sc = sr::SimSweep;
        else if (simScenario == L"dual") sc = sr::SimDual;
        else if (simScenario == L"pulse") sc = sr::SimPulse;
        else {
            fwprintf(stderr, L"--simulate must be sweep, dual or pulse\n");
            rc = 1;
        }
        if (rc == 0) rc = RunSimulate(cfg, sc);
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
