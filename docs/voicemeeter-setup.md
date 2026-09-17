# Voicemeeter Potato Setup / Voicemeeter Potato 配置指南

[English](#english) | [中文](#中文)

---

## English

Free signal path (no driver signing needed, works with Vanguard/ACE):

```
Game → "Voicemeeter Input" (7.1) → Potato strip → B1 bus (8ch, Normal mode)
     → SoundRadar.exe captures "Voicemeeter Out B1"
     → right-ear mono downmix (engine) → your headphones
     → overlay radar
```

Voicemeeter carries the 8 channels. SoundRadar does the downmix and the overlay. Voicemeeter's own Mix Down outputs stereo, so it cannot sum everything into the right ear — keep the downmix in SoundRadar.

### Steps

1. Install Voicemeeter Potato from vb-audio.com (donationware). Reboot.
2. Open Potato. In its Menu, enable "Run on Windows startup".
3. Windows: Settings > System > Sound > Playback. Set "Voicemeeter Input (VB-Audio Voicemeeter VAIO)" as default. Then Configure > 7.1 Surround.
4. In your game, select "Voicemeeter Input" as the audio output if the game has its own device setting.
5. In Potato, find the virtual input strip "Voicemeeter VAIO" (usually the rightmost strip). On this strip: turn OFF A1–A5, turn ON B1 only.
6. B1 bus mode must stay "Normal" (this keeps the 8 channels as-is). Right-click the bus mode label under the B1 fader to check.
7. Start `SoundRadar.exe --tray`. It finds "Voicemeeter Out B1" automatically, mixes all 8 channels into your right ear, and draws the radar.

### Checks

- `SoundRadar.exe --list-devices` must show "Voicemeeter Out B1 ..." with `(selected)`.
- Too quiet or clipping: lower the VAIO strip fader to about -6 dB. The engine soft-clips, but headroom helps.
- Wrong capture device: set `capture_device` in `%APPDATA%\SoundRadar\config.json` to any substring of the endpoint name.
- Potato must stay open while you play.

---

## 中文

免费信号路径（不需要驱动签名，与 Vanguard/ACE 完全兼容）:

```
游戏 → "Voicemeeter Input"(7.1)→ Potato 输入条 → B1 总线(8 声道,Normal 模式)
     → SoundRadar.exe 捕获 "Voicemeeter Out B1"
     → 右耳单声道下混(引擎完成)→ 你的耳机
     → Overlay 雷达
```

分工：Voicemeeter 只负责搬运 8 个声道；下混和雷达都由 SoundRadar 完成。注意 Voicemeeter 自带的 Mix Down 输出的是立体声（左右分离），没法把所有声音塞进右耳，所以下混必须用 SoundRadar。

### 步骤

1. 从 vb-audio.com 下载安装 Voicemeeter Potato（捐赠制免费）。安装后重启电脑。
2. 打开 Potato，点右上角 Menu，勾选 "Run on Windows startup"（开机自启）。
3. Windows:设置 > 系统 > 声音 > 播放。把 "Voicemeeter Input (VB-Audio Voicemeeter VAIO)" 设为默认设备。然后点"配置"，选 7.1 环绕声。
4. 游戏里如果有独立的音频输出设备选项，选 "Voicemeeter Input"。
5. 在 Potato 界面找到 "Voicemeeter VAIO" 虚拟输入条（通常是最右边那条）。在这条上：关掉 A1–A5，只打开 B1。
6. B1 总线的模式必须保持 "Normal"（这样 8 个声道原样传输）。检查方法：右键点 B1 推子上方的模式标签。
7. 运行 `SoundRadar.exe --tray`。它会自动找到 "Voicemeeter Out B1"，把 8 个声道全部混进你的右耳，并画出雷达。

### 排查

- 运行 `SoundRadar.exe --list-devices`，应显示 "Voicemeeter Out B1 ..." 并带 `(selected)` 标记。
- 声音太小或爆音：把 VAIO 输入条的推子降到 -6 dB 左右。引擎自带软限幅，但留点余量更好。
- 捕获了错误的设备：编辑 `%APPDATA%\SoundRadar\config.json` 里的 `capture_device`，填设备名的任意子串。
- 玩游戏时 Potato 窗口必须保持打开（或最小化），关掉它音频就断了。
