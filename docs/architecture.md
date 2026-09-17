# Architecture / 架构

[English](#english) | [中文](#中文)

---

## English

FPS-SoundRadar shows the direction of game sounds on screen. It serves a player who is deaf in the left ear. Two binaries do the work:

1. **SoundRadar VAD** (kernel driver, `driver/`). A virtual 7.1 sound card derived from the Microsoft sysvad sample (MS-PL license). The game renders 8-channel PCM to it. The driver copies every render byte into a kernel ring buffer. Its loopback capture endpoint plays the ring out to user mode. No virtual-cable products are used.
2. **SoundRadar.exe** (user mode, `engine/` + `overlay/`, Apache-2.0). One process, three jobs:
   - capture the 7.1 stream from the loopback endpoint (WASAPI),
   - downmix to the real headphones — RIGHT_MONO mode sums all 8 channels into the right ear with adjustable weights, STEREO mode does a standard 7.1-to-2.0 downmix,
   - draw the direction overlay.

### Data flow

```
Game → Windows audio engine → SoundRadar VAD render pin (8ch PCM)
                                    │ driver ring buffer
                                    ▼
              SoundRadar VAD loopback capture pin
                                    │ WASAPI capture (event-driven)
                                    ▼
              SoundRadar.exe ──► per-channel analysis ──► overlay (radar + edge bands)
                        │
                        └─► downmix (RIGHT_MONO / STEREO) ──► WASAPI render ──► headphones
```

### Direction display rules

- 8 channels map to 8 fixed directions: FL FR C LFE BL BR SL SR.
- Each channel owns one radar sector and one edge-band segment. Levels never merge. Two loud channels at once show two sectors, never a phantom center.
- Color by relative loudness: green = far/weak, yellow = mid, red = near/loud. Thresholds are configurable.
- After a sound stops, the sector fades out over 300–500 ms. A one-pole smoothing filter stops flicker.

### Latency budget (end to end ≤ 30 ms)

| Stage | Budget |
|---|---|
| Driver render timer + ring | ~2 ms |
| Capture client buffer (shared, 10 ms requested) | ≤ 10 ms |
| Engine ring + downmix | ~2 ms |
| Render output (exclusive mode, 5–10 ms buffer) | ≤ 10 ms |
| Safety margin | ~6 ms |

`SoundRadar.exe --measure` reports the real numbers. `docs/latency.md` describes the method.

### Sound classification (experimental)

Heuristics only, no machine learning:
- Footstep: energy concentrated at 100–300 Hz, short bursts, periodic repeats → amber marker.
- Gunshot: broadband, high transient, single burst → red flash marker.
- Everything else: unclassified. The UI labels this feature "experimental".

---

## 中文

FPS-SoundRadar 把游戏声音的方向显示在屏幕上，为左耳失聪的玩家设计。两个程序完成全部工作：

1. **SoundRadar VAD**（内核驱动，`driver/`）。基于微软 sysvad 示例（MS-PL 协议）改造的虚拟 7.1 声卡。游戏把 8 声道 PCM 输出给它。驱动把渲染数据逐字节复制进内核环形缓冲区，再由回路捕获端点送给用户态。不使用任何成品虚拟声卡软件。
2. **SoundRadar.exe**（用户态，`engine/` + `overlay/`，Apache-2.0）。一个进程干三件事：
   - 用 WASAPI 从回路端点捕获 7.1 音频流；
   - 下混到真实耳机——"右耳单声道"模式把 8 个声道按可调权重全部混入右声道，"立体声"模式做标准 7.1→2.0 下混；
   - 绘制方向 Overlay。

### 数据流

```
游戏 → Windows 音频引擎 → SoundRadar VAD 渲染端点（8 声道 PCM）
                                    │ 驱动内环形缓冲区
                                    ▼
              SoundRadar VAD 回路捕获端点
                                    │ WASAPI 捕获（事件驱动）
                                    ▼
              SoundRadar.exe ──► 逐声道分析 ──► Overlay（雷达 + 屏幕边缘条带）
                        │
                        └─► 下混（右耳单声道 / 立体声）──► WASAPI 渲染 ──► 耳机
```

### 方向显示规则

- 8 声道对应 8 个固定方向：前左、前右、中置、低音、后左、后右、侧左、侧右。
- 每个声道独占一个雷达扇区和一段边缘条带。强度永不合并。两个方向同时发声显示两个扇区，绝不显示为虚假的正前方。
- 颜色按相对响度分级：绿=远/弱，黄=中，红=近/响。阈值可调。
- 声音停止后扇区在 300–500 ms 内渐隐。一阶平滑滤波防止闪烁。

### 延迟预算（端到端 ≤ 30 ms）

| 环节 | 预算 |
|---|---|
| 驱动渲染定时器 + 环形缓冲 | ~2 ms |
| 捕获端缓冲（共享模式，请求 10 ms） | ≤ 10 ms |
| 引擎环形缓冲 + 下混 | ~2 ms |
| 渲染输出（独占模式，5–10 ms 缓冲） | ≤ 10 ms |
| 余量 | ~6 ms |

`SoundRadar.exe --measure` 输出实测数据。测量方法见 `docs/latency.md`。

### 声音分类（实验性）

纯启发式，无机器学习：
- 疑似脚步：能量集中在 100–300 Hz、短促、周期性重复 → 琥珀色标记。
- 疑似枪声：宽带、高瞬态、单次爆发 → 红色闪烁标记。
- 其余不分类。界面上标注"实验性"。
