// main.cpp - SoundRadar engine: capture -> analyze -> downmix -> render,
// plus overlay window, tray menu, and headless verification modes.
//
// Thread model:
//   capture thread : WASAPI packet -> analysis (8ch, independent) -> ring
//   render thread  : ring -> Downmix8To2 (mode from atomic, tray hot-swap) -> submit
//   overlay thread : SharedMeters snapshot -> DComp/D2D radar + edge bands
//   main thread    : tray icon + message loop, config, shutdown
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../overlay/overlay.h"
#include "analysis.h"
#include "capture.h"
#include "classify.h"
#include "config.h"
#include "downmix.h"
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

void PrintUsage() {
    std::printf(
        "SoundRadar - 7.1 loopback capture, metering, right-mono/stereo downmix, overlay\n"
        "\n"
        "usage: SoundRadar.exe [options]\n"
        "  (no args)              run engine + overlay + tray in the console\n"
        "  --tray                 run hidden (no console), tray icon only\n"
        "  --selftest             DSP self-tests (no audio devices needed), exit 0/1\n"
        "  --classifytest         sound classification tests (experimental), exit 0/1\n"
        "  --overlaytest [bmp]    overlay checks: ex-style, CPU active/idle, screenshot\n"
        "  --simulate <scenario>  sweep | dual | pulse; feeds synthetic meters ~12 s\n"
        "  --simulate-screenshot <file.bmp>  render one dual frame to a BMP\n"
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
    // mark the capture endpoint the current selection rule would pick
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
        std::printf("  %s%s\n", sr::ToUtf8(d.name).c_str(), d.isDefault ? "  [default]" : "");
}

void PrintMeterLine(const sr::AnalysisFrame& fr) {
    static const char* names[sr::kChannels] = { "FL", "FR", "C ", "LFE", "BL", "BR", "SL", "SR" };
    std::printf("\r");
    for (int c = 0; c < sr::kChannels; ++c)
        std::printf("%s %.2f%s  ", names[c], fr.level[c], fr.peak[c] ? "*" : " ");
    std::printf("%s   ", fr.active ? "ACTIVE" : "idle  ");
    std::fflush(stdout);
}

// Prints the overlay window ex-style and checks every required flag.
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

// Full app: audio pipeline + overlay + tray. Used by both console and --tray.
int RunApp(sr::AppConfig& cfg, const std::wstring& configPath, bool trayMode) {
    sr::ComInit com;
    if (!com.Ok()) {
        fwprintf(stderr, L"COM init failed\n");
        return 1;
    }

    sr::g_downmixMode.store(static_cast<int>(cfg.downmix.mode));
    sr::g_classifyEnabled.store(cfg.classifyEnabled);

    sr::CaptureClient cap;
    sr::RenderClient ren;
    sr::RingBuffer ring(8192);
    sr::Analyzer analyzer(cfg.analysis);
    sr::Classifier8 classifier; // experimental, per-channel, capture thread
    sr::SharedMeters meters;
    sr::Overlay overlay;
    std::thread capThread, renThread;
    bool pipelineRunning = false;

    // Starts capture + render threads. Safe to retry after an init failure:
    // a failed Init leaves no threads behind.
    auto startPipeline = [&](bool verbose) -> bool {
        if (pipelineRunning) return true;
        std::wstring err;
        if (!cap.Init(cfg.captureDevice, err)) {
            if (verbose) std::fprintf(stderr, "%s\n", sr::ToUtf8(err).c_str());
            return false;
        }
        if (!ren.Init(cfg.outputDevice, err)) {
            if (verbose)
                std::fprintf(stderr, "render init failed: %s\n", sr::ToUtf8(err).c_str());
            return false;
        }
        const sr::CaptureClient::Format& cf = cap.GetFormat();
        const sr::RenderClient::Format& rf = ren.GetFormat();
        std::printf("capture : %s (%u Hz, %u ch, buffer %.1f ms)\n",
                    sr::ToUtf8(cap.DeviceName()).c_str(), cf.sampleRate, cf.channels,
                    cf.bufferFrames * 1000.0 / cf.sampleRate);
        std::printf("render  : %s (%s, %u Hz, %u ch, buffer %.1f ms)\n",
                    sr::ToUtf8(ren.DeviceName()).c_str(), rf.exclusive ? "EXCLUSIVE" : "shared",
                    rf.sampleRate, rf.channels, rf.bufferMs);
        std::printf("running - tray icon for controls, Ctrl+C to stop\n\n");

        if (cfg.overlay.enabled) overlay.Start(cfg.overlay, &meters, g_quit);

        capThread = std::thread([&] {
            cap.Run([&](const float* frames, uint32_t n, LONGLONG) {
                sr::AnalysisFrame fr;
                analyzer.Process(frames, n, fr); // pre-downmix, full 8ch
                classifier.Process(frames, n);   // per-channel, independent
                {
                    std::lock_guard<std::mutex> lk(meters.mu);
                    meters.frame = fr;
                    for (int c = 0; c < 8; ++c)
                        meters.classes[c] = static_cast<uint8_t>(classifier.ClassOf(c));
                }
                ring.Write(frames, n);
            }, g_quit);
        });

        renThread = std::thread([&] {
            std::vector<float> in8, stereo;
            ren.Run([&](float* out, uint32_t frames) {
                size_t need8 = static_cast<size_t>(frames) * 8;
                if (in8.size() < need8) {
                    in8.resize(need8);
                    stereo.resize(static_cast<size_t>(frames) * 2);
                }
                size_t got = ring.Read(in8.data(), frames);
                if (got < frames) // underrun: pad with silence
                    std::memset(in8.data() + got * 8, 0, (frames - got) * 8 * sizeof(float));
                sr::DownmixConfig d = cfg.downmix;
                d.mode = static_cast<sr::DownmixMode>(sr::g_downmixMode.load()); // tray hot-swap
                sr::Downmix8To2(in8.data(), stereo.data(), frames, d);
                std::memcpy(out, stereo.data(), frames * 2 * sizeof(float));
            }, g_quit);
        });

        pipelineRunning = true;
        return true;
    };

    sr::Tray tray;
    sr::Tray::Handlers handlers;
    handlers.onMode = [&](int m) {
        sr::g_downmixMode.store(m);
        cfg.downmix.mode = static_cast<sr::DownmixMode>(m);
        sr::SaveConfig(configPath, cfg);
    };
    handlers.onOverlay = [&](bool on) {
        // Overlay off = render thread fully stopped, resources destroyed.
        // The audio path above is untouched either way.
        if (on && pipelineRunning) overlay.Start(cfg.overlay, &meters, g_quit);
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

    int ticksUntilRetry = 0;
    handlers.onTick = [&] {
        if (!pipelineRunning) {
            // The device may appear later (boot order, driver install).
            // Retry silently every 15 s.
            if (++ticksUntilRetry >= 30) {
                ticksUntilRetry = 0;
                if (startPipeline(false))
                    tray.Notify(L"SoundRadar",
                                L"音频设备已连接,开始工作。 Audio device connected.");
            }
            return;
        }
        if (!GetConsoleWindow()) return;
        sr::AnalysisFrame fr;
        {
            std::lock_guard<std::mutex> lk(meters.mu);
            fr = meters.frame;
        }
        PrintMeterLine(fr);
    };
    tray.Init(cfg, handlers);

    if (!startPipeline(true)) {
        if (!trayMode) return 1; // console mode: fail fast with the message above
        tray.Notify(L"SoundRadar 声纹雷达",
                    L"未找到音频捕获设备,每 15 秒自动重试。\n"
                    L"免费方案:安装 Voicemeeter Potato 并把游戏输出设为 Voicemeeter Input。\n"
                    L"或安装本仓库的 SoundRadar VAD 驱动。\n"
                    L"No capture device found. Retrying every 15 s.");
    }

    tray.Run(g_quit); // blocks until quit

    std::printf("\nshutting down...\n");
    tray.Shutdown();
    overlay.Stop();
    if (capThread.joinable()) capThread.join();
    if (renThread.joinable()) renThread.join();
    if (ring.Overruns() > 0)
        std::printf("warning: ring dropped %llu frames (render could not keep up)\n",
                    (unsigned long long)ring.Overruns());
    return 0;
}

// Headless overlay verification: window flags + render thread liveness, ~12 s.
int RunSimulate(sr::AppConfig cfg, sr::SimScenario scenario) {
    cfg.overlay.enabled = true; // simulation exists to exercise the overlay
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

    Sleep(1200); // let a sweep channel reach full level
    uint64_t f0 = overlay.FramesDrawn();
    Sleep(1200);
    uint64_t f1 = overlay.FramesDrawn();
    bool alive = overlay.IsRunning() && f1 > f0;
    std::printf("render thread alive: %s (%llu frames drawn in 1.2 s window)\n",
                alive ? "yes" : "NO", (unsigned long long)(f1 - f0));

    // run the scenario for ~12 s total
    for (int i = 0; i < 90 && WaitForSingleObject(g_quit, 100) == WAIT_TIMEOUT; ++i) {
    }

    sim.Stop();
    overlay.Stop();
    bool ok = styleOk && alive;
    std::printf("simulate: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// One-command overlay CI check: ex-style, CPU active/idle, screenshots.
// Screenshots written next to shotPath: <base>-dual.bmp/-sweep.bmp/-pulse.bmp.
int RunOverlayTest(sr::AppConfig cfg, const std::wstring& shotPath) {
    cfg.overlay.enabled = true;
    sr::SharedMeters meters;
    sr::Overlay overlay;
    overlay.Start(cfg.overlay, &meters, g_quit);

    HWND hwnd = WaitForOverlayWindow(overlay, 3000);
    bool styleOk = hwnd && CheckExStyle(hwnd);

    sr::Simulator sim(sr::SimDual, &meters, g_quit);
    sim.Start();

    Sleep(1500); // dual levels at steady state, overlay at 60 fps
    overlay.ResetStats();
    Sleep(4000);
    sr::Overlay::Stats active = overlay.GetStats();

    sim.SetSilent(true);
    Sleep(2000); // levels fade out, overlay drops to idle polling
    overlay.ResetStats();
    Sleep(3000);
    sr::Overlay::Stats idle = overlay.GetStats();

    std::printf("overlay render thread CPU: active %.3f%% (%llu frames in window), "
                "idle %.3f%%\n",
                active.activeCpuPct, (unsigned long long)active.frames, idle.idleCpuPct);

    // three scenario screenshots; dual/pulse also show classification markers
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    auto baseName = [&](const wchar_t* tag) {
        size_t dot = shotPath.find_last_of(L'.');
        std::wstring stem = dot == std::wstring::npos ? shotPath : shotPath.substr(0, dot);
        return stem + L"-" + tag + L".bmp";
    };
    bool shot = true;
    {
        float dualLevels[8] = {};
        dualLevels[0] = 0.8f; // FL
        dualLevels[5] = 0.8f; // BR
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
        sweepLevels[7] = 0.85f; // SR mid-sweep
        std::wstring p = baseName(L"sweep");
        bool ok = sr::RenderSceneToFile(p, w, h, sweepLevels, nullptr, cfg.overlay);
        std::printf("screenshot: %s (%s)\n", sr::ToUtf8(p).c_str(), ok ? "written" : "FAILED");
        shot = shot && ok;
    }
    {
        float pulseLevels[8] = {};
        pulseLevels[6] = 0.9f; // SL burst
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
    bool framesOk = active.frames > 100; // ~60 fps over the 4 s window
    bool ok = styleOk && cpuOk && framesOk && shot;
    std::printf("overlaytest: style=%s cpu=%s frames=%s screenshot=%s -> %s\n",
                styleOk ? "ok" : "FAIL", cpuOk ? "ok" : "FAIL", framesOk ? "ok" : "FAIL",
                shot ? "ok" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring configPath = sr::DefaultConfigPath();
    std::wstring outputOverride;
    std::wstring modeOverride;
    std::wstring screenshotPath;
    std::wstring simScenario;
    std::wstring overlayTestShot;
    bool selftest = false, measure = false, measureLoopback = false, list = false;
    bool trayMode = false, overlayTest = false, classifyTest = false;
    int panTestSeconds = -1; // -1 = flag not given, 0 = once, >0 = loop budget

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

    if (argc == 1) trayMode = true; // double-click from Explorer: tray app, no console
    if (trayMode) FreeConsole(); // started via Run key / Explorer: no console window

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
        // one dual-scenario frame at primary-monitor size, with class markers
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
        rc = RunApp(cfg, configPath, trayMode);
    }

    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    CloseHandle(g_quit);
    g_quit = nullptr;
    return rc;
}
