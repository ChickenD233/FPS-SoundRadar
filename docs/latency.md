# Latency Measurement / 延迟测量方法

[English](#english) | [中文](#中文)

---

## English

Target: end-to-end ≤ 30 ms, from the game writing PCM to the virtual device, to the downmixed signal leaving the real headphone output.

### What `SoundRadar.exe --measure` reports

1. **Capture side**: WASAPI device period (actual), buffer size in ms, bytes per packet seen at runtime.
2. **Render side**: mode (exclusive or shared), actual device period, buffer size in ms.
3. **Pipeline**: engine ring depth in ms at the time of the report.
4. **Estimated end-to-end** = capture buffer + ring depth + render buffer + both device periods.

### Loopback round-trip test (`--measure-loopback`)

1. The tool opens the SoundRadar speaker endpoint as a WASAPI render client.
2. It plays a train of short clicks (20 ms silence, 1 ms click) and records the submit timestamp of each click (QueryPerformanceCounter).
3. The same process captures the loopback stream and detects each click's arrival.
4. The difference (arrival minus submit) measures: Windows audio engine + driver ring + capture client buffering.
5. Add the render-path numbers from `--measure` to get the full game-to-headphone estimate.

### Manual verification (optional, most exact)

1. Connect a second PC's line-in to the headphone output.
2. Run `--measure-loopback` and record both the virtual render submit (PC clock) and the analog arrival on the second PC (Audacity recording).
3. The total includes the DAC and cable, which software timestamps cannot see. Expect 1–3 ms extra.

### CPU measurement

1. Open Task Manager > Details > SoundRadar.exe, or run `Get-Counter '\Process(SoundRadar*)\% Processor Time'`.
2. Play a busy game scene for 60 s.
3. The average must stay under 5% on one core.

---

## 中文

目标：端到端 ≤ 30 ms，从游戏把 PCM 写入虚拟设备，到下混信号离开真实耳机输出。

### `SoundRadar.exe --measure` 输出的内容

1. **捕获侧**:WASAPI 实际设备周期、缓冲大小（毫秒）、运行时每个数据包的字节数。
2. **渲染侧**：模式（独占或共享）、实际设备周期、缓冲大小。
3. **管线**：报告时刻引擎环形缓冲的深度（毫秒）。
4. **端到端估算** = 捕获缓冲 + 环形缓冲深度 + 渲染缓冲 + 两侧设备周期。

### 回路往返测试（`--measure-loopback`)

1. 工具以 WASAPI 渲染客户端身份打开 SoundRadar 扬声器端点。
2. 播放一列短促咔哒声（20 ms 静音 + 1 ms 咔哒），记录每次提交的时间戳（QueryPerformanceCounter)。
3. 同一进程捕获回路音频流，检测每个咔哒声的到达时刻。
4. 到达时刻减提交时刻，测得：Windows 音频引擎 + 驱动环形缓冲 + 捕获客户端缓冲的总延迟。
5. 加上 `--measure` 的渲染侧数据，得到完整的"游戏到耳机"估算值。

### 手动验证（可选，最精确）

1. 把耳机输出接到另一台电脑的线路输入。
2. 运行 `--measure-loopback`，同时用第二台电脑（Audacity）录制模拟信号到达时刻。
3. 这个结果包含 DAC 和线材延迟，软件时间戳看不到这部分。预计多出 1–3 ms。

### CPU 占用测量

1. 任务管理器 > 详细信息 > SoundRadar.exe，或运行 `Get-Counter '\Process(SoundRadar*)\% Processor Time'`。
2. 玩 60 秒战斗激烈的场景。
3. 单核平均占用必须低于 5%。
