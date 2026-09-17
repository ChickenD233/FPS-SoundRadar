# Verification / 验收记录

[English](#english) | [中文](#中文)

---

## English

Test date: 2026-09-18. Machine: Windows 11 x64 (23H2), VS2022 BuildTools 17.14, WDK 10.0.26100.6584.

### Acceptance matrix

| # | Test | Status | Evidence |
|---|---|---|---|
| 1 | Tones panned to each 7.1 channel map to the right overlay sector | Offline PASS, on-device PENDING | `--simulate sweep` + `--overlaytest` screenshots show each sector lighting alone. On-device: install the driver, run `--pan-test`, watch the radar |
| 2 | FL+BR at once shows two independent sectors | PASS | `--selftest` "channel independence (FL+BR, no center merge)" + dual screenshot |
| 3 | All 8 channels audible in the right ear after downmix | Offline PASS, on-device PENDING | `--selftest` "right-mono carries all 8 channels (Goertzel)". On-device: `--pan-test` step 3, listen to the right ear |
| 4 | Overlay topmost, click-through, no focus steal in a borderless game | Code-verified, on-device PENDING | `--overlaytest` asserts the ex-style: WS_EX_TRANSPARENT, LAYERED, TOPMOST, NOACTIVATE, TOOLWINDOW all set; WM_NCHITTEST returns HTTRANSPARENT. Final check needs a real game window |
| 5 | Overlay off = zero impact on the audio path | PASS by design | Overlay runs on its own thread fed by a mutex-protected snapshot. The toggle stops that thread; the capture/downmix/render path never touches overlay state. Downmix path has no dependency on UI code |
| 6 | End-to-end latency ≤ 30 ms, CPU < 5% | Partially measured | Exclusive render mode negotiated on real hardware: 5 ms buffer, 2.02 ms device period. Budget: capture 10 ms + ring ~2 ms + render 5 ms + periods ≈ ≤ 22 ms. Overlay render thread measured 0.0% CPU active/idle. Full `--measure` and `--measure-loopback` numbers need the installed driver |

### Blocked for user action (with exact steps)

The driver is not installed on the build machine because test mode would break Vanguard/ACE, and attestation signing needs your EV certificate.

1. Pick a signing path in `docs/signing.md`.
2. Dev PC or VM: run `scripts\install-driver.ps1` as administrator and reboot.
3. Gaming PC: run `scripts\attestation-sign.ps1` with your EV certificate, submit in the Partner Center, then install with `pnputil /add-driver`.
4. After install: set "SoundRadar Virtual 7.1 (Speaker)" as the default output, start `SoundRadar.exe --tray`.
5. Run `SoundRadar.exe --pan-test` for tests 1 and 3, `--measure` and `--measure-loopback` for test 6, and open a borderless game for test 4.

---

## 中文

测试日期：2026-09-18。机器：Windows 11 x64 (23H2)、VS2022 BuildTools 17.14、WDK 10.0.26100.6584。

### 验收矩阵

| # | 测试项 | 状态 | 证据 |
|---|---|---|---|
| 1 | 逐声道声像与 Overlay 扇区一一对应 | 离线通过，真机待测 | `--simulate sweep` + `--overlaytest` 截图显示每个扇区单独点亮。真机：装好驱动后运行 `--pan-test` 观察雷达 |
| 2 | 前左+后右同时发声显示两个独立扇区 | 通过 | `--selftest` "channel independence (FL+BR, no center merge)" + dual 截图 |
| 3 | 下混后 8 声道在右耳逐一清晰可闻 | 离线通过，真机待测 | `--selftest` "right-mono carries all 8 channels (Goertzel)"。真机：`--pan-test` 第三步，用右耳听 |
| 4 | 无边框游戏中 Overlay 置顶、点击穿透、不抢焦点 | 代码级验证，真机待测 | `--overlaytest` 断言窗口样式：TRANSPARENT、LAYERED、TOPMOST、NOACTIVATE、TOOLWINDOW 全部置位；WM_NCHITTEST 返回 HTTRANSPARENT。最终确认需要真实游戏窗口 |
| 5 | 关闭声纹后音频链路零影响 | 设计保证 | Overlay 在独立线程，通过快照读数据。开关只停渲染线程；捕获/下混/渲染链路完全不依赖界面代码 |
| 6 | 端到端 ≤ 30 ms、CPU < 5% | 部分实测 | 独占模式在真实声卡上协商成功：5 ms 缓冲、2.02 ms 设备周期。预算：捕获 10 ms + 环形缓冲 ~2 ms + 渲染 5 ms + 周期 ≈ ≤ 22 ms。Overlay 渲染线程实测 CPU 0.0%。完整 `--measure` 和 `--measure-loopback` 数据需要装好驱动后测 |

### 需要用户操作的步骤

构建机没有装驱动，因为测试模式会导致 Vanguard/ACE 拦截游戏，而 attestation 签名需要你的 EV 证书。

1. 在 `docs/signing.md` 里选签名路径。
2. 开发机或虚拟机：管理员运行 `scripts\install-driver.ps1`，重启。
3. 游戏机器：运行 `scripts\attestation-sign.ps1`（需 EV 证书），在合作伙伴中心提交，再用 `pnputil /add-driver` 安装。
4. 装好后：把默认输出设备设为 "SoundRadar Virtual 7.1 (Speaker)"，启动 `SoundRadar.exe --tray`。
5. 运行 `--pan-test` 验证第 1、3 条，`--measure` 和 `--measure-loopback` 验证第 6 条，打开无边框游戏验证第 4 条。
