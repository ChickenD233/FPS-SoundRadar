# FPS-SoundRadar

[English](#english) | [中文](#中文)

---

## English

Game-audio direction radar for players who are deaf in one ear. Built for Valorant, Delta Force, and CS2 on Windows 11 x64.

FPS-SoundRadar gives you two things:

1. **A visual radar for sound direction.** A click-through overlay points small chevron arrows on an invisible ring toward each live sound source, plus bright bands along the screen edges. Each direction stays independent: front-left plus back-right shows two arrows, never a fake "front center".
2. **Downmix with three output modes.** Stereo keeps the 7.1 spatial image. Right-ear mono and left-ear mono send all 8 channels into one ear with adjustable weights, so no sound from the rear, center, or side channels gets lost. Switch live, no restart.

### How it works

```
Game → SoundRadar VAD (virtual 7.1 driver) → loopback capture → SoundRadar.exe
                                                              ├─ overlay (radar + edge bands)
                                                              └─ downmix → your headphones
```

- `driver/` — SoundRadar VAD. A virtual 7.1 audio driver derived from the Microsoft sysvad sample (MS-PL license). It reports a 7.1 speaker endpoint and copies the render stream into a loopback capture endpoint. No third-party virtual-cable software is used.
- `engine/` + `overlay/` — SoundRadar.exe. One background process: WASAPI capture, downmix engine, per-channel analysis, and the Direct2D overlay. Runs from the tray, starts with Windows (optional).

### Features

- Direction arrows on an invisible ring (continuous 360° tracking, one arrow per simultaneous source) and screen-edge bands, color-graded by loudness (green far, yellow mid, red near; thresholds adjustable).
- Adaptive detection. The engine measures the ambient noise floor per channel over a sliding window and normalizes against it, so a quiet game mix (for example 40% global volume) still trips the arrows. The sensitivity slider feeds that detection gain, not a display-only multiply. Footsteps 15-20 dB above the ambient floor are detected. The old fixed thresholds stopped at -24 dBFS.
- A control the user touched keeps the user's value. The app pushes its stored state to the window twice a second; before this, that packet overwrote anything not yet applied, so a switch flipped back and a number field reverted while the user was still editing it.
- Every slider reads low to high from left to right. The two ends carry the words 低 and 高 in fluorescent blue and amber, the track shows the same ramp, and the hint under the slider uses those colors on the same words. The readable cue and the text cue cannot disagree. The sensitivity slider sets arrow brightness and how soon the color turns red. The detection threshold decides what gets detected. The color scale is relative to the measured ambient level, so a quiet mix stays visible instead of reading black.
- Noise gate. Ambient hiss and fan noise produce no arrows and no glow, so a direction indicator always means real sound.
- Frontal merge. With the merge toggle on, the front-left and front-right pair always draws ONE arrow dead ahead, even when the two channels differ in level.
- Fast release. Arrows reach zero 300-700 ms after the sound stops, so no mark lingers over an empty scene.
- 50 ms arrow glide, 250 ms display fade, 120 ms noise-gate release. No flicker.
- Overlay refresh: up to ~120 fps while sound is present, ~4 fps when idle.
- `SoundRadar.exe --sselftest` runs the detection tests (quiet mix, noise rejection, fade, recovery) without audio devices.
- Overlay: topmost, fully click-through, never steals focus. CPU under 5% while active, near zero when idle.
- Two downmix modes, switchable live from the main window or the tray menu with no restart:
  - **Stereo (7.1 spatial image)** - the default. 7.1 folds to two channels and the left/right image is preserved.
  - **Mono** - all eight channels sum into one signal on both ears. The per-channel weight sliders apply here.
- Experimental view compensation (off by default). It rotates the drawn sound directions by the view angle you turn, so an arrow shows where a sound sits relative to the view you have now. That cancels the perceived lag on a delayed footstep or gunshot. The mouse-to-degrees factor comes from the same cm/360 figure that pointer-sensitivity sites use to compare games: one count is 360 / (cm360 / 2.54 * dpi) degrees, with a correction slider and an invert switch for games that do not use raw input. The direction set has 7 entries, so the rotation moves in 30 or 60 degree steps.
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

### Free path (no driver): VB-CABLE

The simplest setup, and the right one for machines with kernel anti-cheat (Vanguard / ACE): no unsigned driver, nothing in test mode. [VB-CABLE](https://vb-audio.com/Cable/) (donationware, digitally signed) carries the 7.1 signal.

1. Install VB-CABLE, reboot. In Windows sound settings set the playback device "CABLE In 16 Ch" (or "CABLE Input") as default, then Configure it as 7.1 surround.
2. Run `SoundRadar.exe`. Capture auto-selects "CABLE Output" (8 channels); set Output to your real headphones and Apply. The engine does the downmix.

Fallback order for capture when `capture_device` is the default `"SoundRadar"`: VAD loopback → CABLE Output → Voicemeeter Out B1 → Voicemeeter Output. A custom substring can be set in `%APPDATA%\SoundRadar\config.json`.

### Alternative: Voicemeeter Potato

[Voicemeeter Potato](https://vb-audio.com/Voicemeeter/potato.htm) (donationware) can also carry the 7.1 signal and do the downmix itself.

1. Install Voicemeeter Potato. Set the game output device to "Voicemeeter Input". Configure that device as 7.1 in Windows.
2. In Potato: route the input strip to the B1 bus with the 8-channel patch. On the A1 bus (your headphones), mix all channels into the right ear.
3. Run `SoundRadar.exe`. It finds "Voicemeeter Out B1" by itself and shows the overlay.

Notes:

- Potato does the downmix in this setup. The engine downmix becomes optional.
- The capture stream can have fewer than 8 channels. Channels map in FL FR C LFE BL BR SL SR order. Missing channels stay silent on the radar.
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

1. **声音方向可视化**。一个鼠标完全穿透的置顶 Overlay，在隐形圆环上用小箭头实时指向每个声源方向，屏幕四边还有高亮条带。各方向独立显示：前左和后右同时发声就显示两个箭头，绝不合并成虚假的"正前方"。
2. **三种下混模式**。立体声保留 7.1 空间感；右耳单声道和左耳单声道按可调权重把 8 个声道全部混入一只耳朵，后置、中置、侧置声道的声音一个都不丢。可即时切换，不需要重启。

### 工作原理

```
游戏 → SoundRadar VAD（虚拟 7.1 驱动）→ 回路捕获 → SoundRadar.exe
                                                  ├─ Overlay（雷达 + 屏幕边缘条带）
                                                  └─ 下混 → 你的耳机
```

- `driver/` —— SoundRadar VAD 虚拟 7.1 声卡驱动，基于微软 sysvad 示例（MS-PL 协议）改造。向系统报告 7.1 扬声器端点，并把渲染音频复制到回路捕获端点。不使用任何成品虚拟声卡软件。
- `engine/` + `overlay/` —— SoundRadar.exe，一个后台进程完成：WASAPI 捕获、下混、逐声道分析、Direct2D Overlay。托盘运行，可选开机自启。

### 功能

- 隐形圆环上的方向箭头（360° 连续跟踪，每个声源一个箭头）+ 屏幕四边条带，按响度分级变色（绿=远、黄=中、红=近，阈值可调）。
- 自适应检测。引擎逐声道在滑动窗口内测量环境噪声底，并据此归一化阈值。因此游戏全局音量只有 40% 时，轻微脚步依然能触发箭头。灵敏度滑块直接参与检测增益，不再只是显示放大。比环境噪声底高 15–20 dB 的脚步即可检出；旧的固定阈值下限是 -24 dBFS。
- 用户刚改过的控件以用户的值为准。程序每秒两次把已保存的状态推给界面；在此之前这个状态包会覆盖尚未应用的内容，导致开关自己弹回、数字字段在编辑时被改回。
- 每个滑块都是左低右高：两端标注"低"和"高"，分别是荧光蓝和橙色；轨道是同一条渐变色，滑块下面的提示里同名文字用同一颜色，颜色和文字不会互相矛盾。灵敏度滑块决定箭头亮度和多久变红；能否检测到声音由"检测门限"决定。颜色刻度相对实测环境噪声底，所以低音量混音也能看清，不会一片漆黑。
- 噪声门。环境底噪和风扇声不会画出箭头、也不会发光，所以出现声纹就一定代表真实声音。
- 正前方融合。开启融合开关后，左前+右前永远只画一个正前方箭头，两声道音量不一致时也一样。
- 快速消失。声音停止后 300–700 ms 内声纹归零，不会在空场景上残留。
- 50 ms 箭头滑动、250 ms 显示渐隐、120 ms 噪声门释放，防闪烁。
- Overlay 刷新率：有声时最高约 120 fps，空闲约 4 fps。
- `SoundRadar.exe --sselftest` 无需音频设备即可运行检测测试（轻音混音、噪声抑制、渐隐、恢复）。
- Overlay 置顶、完全点击穿透、不抢焦点。工作时 CPU 低于 5%，空闲接近零。
- 两种下混模式，主窗口或托盘菜单里即时切换，不需要重启：
  - **全景声 Stereo**：默认。7.1 折叠成两声道，保留左右空间感。
  - **单声道 Mono**：8 个声道合成一路，两耳听同一份；逐声道权重滑块在这里生效。
- 实验功能：指针跟随转向（默认关闭）。它按你转过的视角角度反向旋转声纹，让箭头指向"相对当前视角"的方位，用来抵消脚步声/枪声的延迟感。鼠标到角度的换算与灵敏度换算站点用的 cm/360 一致：一个计数 = 360 / (cm360 / 2.54 × dpi) 度，另配校正滑块与反向开关。方向集合只有 7 个，所以旋转以 30°/60° 为一步。
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

### 免费方案（免驱动）：VB-CABLE

最简单的方案，也是装有内核级反作弊（Vanguard / ACE）机器的正确选择：不用未签名驱动、不用开测试模式。[VB-CABLE](https://vb-audio.com/Cable/)（免费捐赠软件，带微软数字签名）负责传输 7.1 信号。

1. 安装 VB-CABLE 并重启。在 Windows 声音设置里把播放设备 "CABLE In 16 Ch"（或 "CABLE Input"）设为默认，然后右键配置扬声器选 7.1 环绕。
2. 运行 `SoundRadar.exe`。捕获会自动选中 "CABLE Output"（8 声道）；输出选你的真实耳机，点应用。下混由引擎完成。

`capture_device` 为默认 `"SoundRadar"` 时捕获的自动匹配顺序：VAD 回路 → CABLE Output → Voicemeeter Out B1 → Voicemeeter Output。可在 `%APPDATA%\SoundRadar\config.json` 里自定义匹配子串。

### 替代方案：Voicemeeter Potato

[Voicemeeter Potato](https://vb-audio.com/Voicemeeter/potato.htm)（免费捐赠软件）也能传输 7.1 信号，并由它自己完成下混。

1. 安装 Voicemeeter Potato。游戏输出设备选 "Voicemeeter Input"，并在 Windows 里把它配置为 7.1。
2. 在 Potato 里：输入条用 8 声道补丁路由到 B1 总线。在 A1 总线（你的耳机）上把所有声道混进右耳。
3. 运行 `SoundRadar.exe`。它会自动找到 "Voicemeeter Out B1" 并显示 Overlay。

说明：

- 此方案由 Potato 完成下混，引擎下混变为可选。
- 捕获流可以少于 8 声道。声道按 FL FR C LFE BL BR SL SR 顺序映射，缺失声道在雷达上保持静默。
- 上面的 SoundRadar VAD 驱动路径仍是自包含方案：不装第三方软件，由引擎完成下混。

### 运行

双击 `SoundRadar.exe` 打开主界面（设备、模式、声纹样式、声道权重、状态栏）。应用按钮保存并即时生效，关闭窗口即最小化到托盘。`SoundRadar.exe --tray` 仅托盘运行（开机自启用此参数）。托盘菜单有 打开主界面、下混模式、声纹开关、实验性分类、开机自启。配置文件在 `%APPDATA%\SoundRadar\config.json`，诊断日志在 `%APPDATA%\SoundRadar\log.txt`。单实例运行：再次启动只会唤起已有窗口。

常用命令行：`--list-devices`、`--mode right-mono|stereo`、`--output <设备名>`、`--selftest`、`--classifytest`、`--guitest`、`--simulate-gui`、`--measure`、`--measure-loopback`、`--pan-test [秒数]`、`--simulate sweep|dual|pulse`、`--overlaytest`。

### 许可证

- 驱动（`driver/sysvad/`):MS-PL，衍生自微软 Windows-driver-samples，见 `driver/SYSVAD-LICENSE.txt`。
- 其余全部：Apache License 2.0，见 `LICENSE` 和 `NOTICE`。
- 两种协议都允许商用。
