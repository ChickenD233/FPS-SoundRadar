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

### Run

`SoundRadar.exe --tray` starts in the tray. The tray menu switches the downmix mode, toggles the overlay and the experimental classification, and controls autostart. Settings live in `%APPDATA%\SoundRadar\config.json`.

Useful CLI flags: `--list-devices`, `--mode right-mono|stereo`, `--output <name>`, `--selftest`, `--measure`, `--measure-loopback`, `--simulate sweep|dual|pulse`, `--overlaytest`, `--classifytest`.

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

### 运行

`SoundRadar.exe --tray` 托盘启动。托盘菜单可切换下混模式、开关 Overlay 和实验性分类、控制开机自启。配置文件在 `%APPDATA%\SoundRadar\config.json`。

常用命令行：`--list-devices`、`--mode right-mono|stereo`、`--output <设备名>`、`--selftest`、`--measure`、`--measure-loopback`、`--simulate sweep|dual|pulse`、`--overlaytest`、`--classifytest`。

### 许可证

- 驱动（`driver/sysvad/`):MS-PL，衍生自微软 Windows-driver-samples，见 `driver/SYSVAD-LICENSE.txt`。
- 其余全部：Apache License 2.0，见 `LICENSE` 和 `NOTICE`。
- 两种协议都允许商用。
