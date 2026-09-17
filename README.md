# FPS-SoundRadar

[English](#english) | [中文](#中文)

---

## English

Game-audio direction radar for players who are deaf in one ear. Built for Valorant, Delta Force, and CS2 on Windows 11 x64.

FPS-SoundRadar gives you two things:

1. **A visual radar for sound direction.** A click-through overlay shows which of the 8 surround channels carries sound right now. Each direction stays independent: front-left plus back-right shows two sectors, never a fake "front center".
2. **Right-ear mono downmix.** All 8 channels mix into the right ear with adjustable weights. No sound from rear, center, or side channels gets lost.

### How it works

```
Game → SoundRadar VAD (virtual 7.1 driver) → loopback capture → SoundRadar.exe
                                                              ├─ overlay (radar + edge bands)
                                                              └─ downmix → your headphones
```

- `driver/` — SoundRadar VAD. A virtual 7.1 audio driver derived from the Microsoft sysvad sample (MS-PL license). It reports a 7.1 speaker endpoint and copies the render stream into a loopback capture endpoint. No third-party virtual-cable software is used.
- `engine/` + `overlay/` — SoundRadar.exe. One background process: WASAPI capture, downmix engine, per-channel analysis, and the Direct2D overlay. Runs from the tray, starts with Windows (optional).

### Features

- 7.1 per-channel radar sectors and screen-edge bands, color-graded by loudness (green far, yellow mid, red near; thresholds adjustable).
- 300–500 ms fade-out and smoothing, no flicker.
- Overlay: topmost, fully click-through, never steals focus. CPU under 5% while active, near zero when idle.
- Downmix modes: right-ear mono (default) and standard stereo.
- Experimental sound classification: footsteps (100–300 Hz, periodic bursts) and gunshots (broadband transient) get distinct markers.
- Overlay and classification switches never touch the audio path.
- Latency target: end-to-end ≤ 30 ms. Measure with `SoundRadar.exe --measure` and `--measure-loopback`. Method: `docs/latency.md`.

### Build

Requirements: Windows 11 x64, VS2022 Build Tools (C++ workload), Windows SDK 10.0.26100, WDK 10.0.26100.6584.

```
powershell -ExecutionPolicy Bypass -File scripts\build-driver.ps1   # driver package
cmake -S . -B build -G "Visual Studio 17 2022" -A x64               # engine + overlay
cmake --build build --config Release
```

`build-driver.ps1` installs a small toolset glue on BuildTools-only machines (files in `driver/toolset-glue/`), builds the driver, stamps the INF, and generates the catalog in `build/driver/Package/`.

### Install the driver

Driver signing decides which script you use. Read `docs/signing.md` first.

- **Gaming PC (Valorant / Delta Force installed)**: attestation signing. Vanguard and ACE refuse to run in test mode. You need an EV certificate and a free Partner Center account. Steps: `scripts/attestation-sign.ps1`.
- **Dev PC or VM**: test signing. Run `scripts/install-driver.ps1` as administrator, reboot, then set "SoundRadar Virtual 7.1 (Speaker)" as the default output device in Windows and in the game.

Remove with `scripts/uninstall-driver.ps1`.

### Free alternative: Voicemeeter Potato

No driver install needed. Voicemeeter Potato (donationware) can carry the 7.1 signal instead of the SoundRadar VAD.

1. Install Voicemeeter Potato. Set the game output device to "Voicemeeter Input". Configure that device as 7.1 in Windows.
2. In Potato: route the input strip to the B1 bus with the 8-channel patch. On the A1 bus (your headphones), mix all channels into the right ear.
3. Run `SoundRadar.exe`. It finds "Voicemeeter Out B1" by itself and shows the overlay.

Notes:

- Potato does the downmix in this setup. The engine downmix becomes optional. This path gives the lowest latency, because Potato mixes before the engine sees the signal.
- The capture stream can have fewer than 8 channels. Channels map in FL FR C LFE BL BR SL SR order. Missing channels stay silent on the radar.
- The config key `capture_device` in `%APPDATA%\SoundRadar\config.json` overrides the capture endpoint substring. Default `"SoundRadar"`: match the VAD loopback first, then Voicemeeter B1, then Voicemeeter Output.
- The SoundRadar VAD driver path above stays the self-contained option: no extra software, and the engine does the downmix.

### Run

Double-click `SoundRadar.exe` to open the main window (devices, mode, overlay style, channel weights, status bar). Apply saves and hot-applies without a restart; closing the window minimizes to the tray. `SoundRadar.exe --tray` starts tray-only (used by autostart). The tray menu has 打开主界面 Open, downmix mode, overlay and classification toggles, and autostart. Settings live in `%APPDATA%\SoundRadar\config.json`; diagnostics in `%APPDATA%\SoundRadar\log.txt`. Only one instance runs at a time — a second launch focuses the existing window.

Useful CLI flags: `--list-devices`, `--mode right-mono|stereo`, `--output <name>`, `--selftest`, `--classifytest`, `--guitest`, `--simulate-gui`, `--measure`, `--measure-loopback`, `--pan-test [seconds]`, `--simulate sweep|dual|pulse`, `--overlaytest`.

### License

- Driver (`driver/sysvad/`): MS-PL, derived from Microsoft Windows-driver-samples. See `driver/SYSVAD-LICENSE.txt`.
- Everything else: Apache License 2.0. See `LICENSE` and `NOTICE`.
- Both licenses allow commercial use.

---

## 中文

为单耳失聪玩家设计的游戏声音方向雷达。支持无畏契约、三角洲行动、CS2，平台 Windows 11 x64。

FPS-SoundRadar 提供两个功能：

1. **声音方向可视化**。一个鼠标完全穿透的置顶 Overlay，实时显示 8 个环绕声道中哪个方向有声音。各方向独立显示：前左和后右同时发声就显示两个扇区，绝不合并成虚假的"正前方"。
2. **右耳单声道下混**。8 个声道按可调权重全部混入右耳。后置、中置、侧置声道的声音一个都不丢。

### 工作原理

```
游戏 → SoundRadar VAD（虚拟 7.1 驱动）→ 回路捕获 → SoundRadar.exe
                                                  ├─ Overlay（雷达 + 屏幕边缘条带）
                                                  └─ 下混 → 你的耳机
```

- `driver/` —— SoundRadar VAD 虚拟 7.1 声卡驱动，基于微软 sysvad 示例（MS-PL 协议）改造。向系统报告 7.1 扬声器端点，并把渲染音频复制到回路捕获端点。不使用任何成品虚拟声卡软件。
- `engine/` + `overlay/` —— SoundRadar.exe，一个后台进程完成：WASAPI 捕获、下混、逐声道分析、Direct2D Overlay。托盘运行，可选开机自启。

### 功能

- 7.1 逐声道雷达扇区 + 屏幕四边条带，按响度分级变色（绿=远、黄=中、红=近，阈值可调）。
- 声音消失后 300–500 ms 渐隐，平滑滤波防闪烁。
- Overlay 置顶、完全点击穿透、不抢焦点。工作时 CPU 低于 5%，空闲接近零。
- 下混模式：右耳单声道（默认）和标准立体声。
- 实验性声音分类：脚步（100–300 Hz 周期性短促爆发）和枪声（宽带高瞬态）有专用标记。
- 声纹开关和分类开关对音频链路零影响。
- 延迟目标：端到端 ≤ 30 ms。用 `SoundRadar.exe --measure` 和 `--measure-loopback` 实测。方法见 `docs/latency.md`。

### 构建

环境要求：Windows 11 x64、VS2022 Build Tools（C++ 工作负载）、Windows SDK 10.0.26100、WDK 10.0.26100.6584。

```
powershell -ExecutionPolicy Bypass -File scripts\build-driver.ps1   # 驱动包
cmake -S . -B build -G "Visual Studio 17 2022" -A x64               # 引擎 + Overlay
cmake --build build --config Release
```

`build-driver.ps1` 会在只有 BuildTools 的机器上自动补装驱动工具集胶水（文件在 `driver/toolset-glue/`)，然后构建驱动、打 INF 时间戳、生成目录文件，输出在 `build/driver/Package/`。

### 安装驱动

签名方式决定用哪个脚本。先读 `docs/signing.md`。

- **游戏机器（装了无畏契约/三角洲行动）**：必须 attestation 签名。Vanguard 和 ACE 在测试模式下拒绝启动游戏。需要 EV 证书和免费的微软合作伙伴中心账户。步骤见 `scripts/attestation-sign.ps1`。
- **开发机或虚拟机**：测试签名。管理员运行 `scripts/install-driver.ps1`，重启，然后在 Windows 和游戏里把默认输出设备选为 "SoundRadar Virtual 7.1 (Speaker)"。

卸载用 `scripts/uninstall-driver.ps1`。

### 免费替代方案：Voicemeeter Potato

不装驱动也能用。Voicemeeter Potato（免费捐赠软件）可以代替 SoundRadar VAD 传输 7.1 信号。

1. 安装 Voicemeeter Potato。游戏输出设备选 "Voicemeeter Input"，并在 Windows 里把它配置为 7.1。
2. 在 Potato 里：输入条用 8 声道补丁路由到 B1 总线。在 A1 总线（你的耳机）上把所有声道混进右耳。
3. 运行 `SoundRadar.exe`。它会自动找到 "Voicemeeter Out B1" 并显示 Overlay。

说明：

- 此方案由 Potato 完成下混，引擎下混变为可选。延迟最低，因为 Potato 在引擎之前完成混音。
- 捕获流可以少于 8 声道。声道按 FL FR C LFE BL BR SL SR 顺序映射，缺失声道在雷达上保持静默。
- 配置文件 `%APPDATA%\SoundRadar\config.json` 里的 `capture_device` 可覆盖捕获端点子串。默认 `"SoundRadar"`：先匹配 VAD 回路，再试 Voicemeeter B1，再试 Voicemeeter Output。
- 上面的 SoundRadar VAD 驱动路径仍是自包含方案：不装第三方软件，由引擎完成下混。

### 运行

双击 `SoundRadar.exe` 打开主界面（设备、模式、声纹样式、声道权重、状态栏）。应用按钮保存并即时生效，关闭窗口即最小化到托盘。`SoundRadar.exe --tray` 仅托盘运行（开机自启用此参数）。托盘菜单有 打开主界面、下混模式、声纹开关、实验性分类、开机自启。配置文件在 `%APPDATA%\SoundRadar\config.json`，诊断日志在 `%APPDATA%\SoundRadar\log.txt`。单实例运行：再次启动只会唤起已有窗口。

常用命令行：`--list-devices`、`--mode right-mono|stereo`、`--output <设备名>`、`--selftest`、`--classifytest`、`--guitest`、`--simulate-gui`、`--measure`、`--measure-loopback`、`--pan-test [秒数]`、`--simulate sweep|dual|pulse`、`--overlaytest`。

### 许可证

- 驱动（`driver/sysvad/`):MS-PL，衍生自微软 Windows-driver-samples，见 `driver/SYSVAD-LICENSE.txt`。
- 其余全部：Apache License 2.0，见 `LICENSE` 和 `NOTICE`。
- 两种协议都允许商用。
