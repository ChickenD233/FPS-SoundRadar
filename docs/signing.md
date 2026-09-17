# Driver Signing Plan / 驱动签名方案

[English](#english) | [中文](#中文)

---

## English

### The decision

| Use case | Signing mode | Works with anti-cheat? |
|---|---|---|
| Development and testing | Test signing | No (Vanguard and ACE block test mode) |
| Daily gaming (Valorant, Delta Force, CS2) | **Attestation signing** | Yes |

Recommendation: use attestation signing for the machine where you play. Use test signing only on a separate PC or VM for development.

### Why test signing fails for gaming

Test signing needs `bcdedit /set testsigning on` and a reboot. In test mode, Windows loads drivers signed with any self-signed certificate. Anti-cheat systems detect this state:

- **Valorant (Vanguard, kernel driver vgk.sys)**: Vanguard checks Secure Boot and test mode. With test mode on, Vanguard refuses to start and Valorant shows error VAN9003/VAN9001-class errors.
- **Delta Force (ACE, Anti-Cheat Expert)**: ACE also loads a kernel driver and rejects systems in test-signing mode.
- **CS2 (VAC)**: VAC is user-mode and tolerates test mode, but the first two games already force the decision.

### Attestation signing (recommended)

Attestation signing is a Microsoft hardware-dashboard service. Microsoft co-signs your driver after you submit it. The result loads on Windows 10/11 x64 with Secure Boot on and test mode off.

Requirements:

1. A Microsoft Partner Center (hardware) account. Registration is free.
2. An EV (Extended Validation) code signing certificate from a public CA (DigiCert, Sectigo, GlobalSign). Cost: roughly US$300–500 per year. The certificate ships on a hardware token.
3. Sign the driver package with your EV certificate.
4. Submit the package to the Partner Center attestation portal. Microsoft returns it co-signed, usually within hours.
5. Attestation-signed drivers cannot use test-only features. This driver is a plain audio driver, so this is not a problem.

Steps are in `scripts/attestation-sign.ps1` (with comments for each portal action that needs a browser).

### Test signing (development only)

`scripts/install-driver.ps1` automates test signing:

1. Creates a self-signed code-signing certificate (`SoundRadarTestCert`).
2. Installs it into the local machine Root and TrustedPublisher stores.
3. Signs `TabletAudioSample.sys` and the catalog file.
4. Enables test mode (`bcdedit /set testsigning on`).
5. Installs the driver with `pnputil`.
6. Asks for a reboot.

`scripts/uninstall-driver.ps1` removes the device, the driver package, the certificate, and can turn test mode off again.

WARNING: Do not enable test mode on a PC where Valorant or Delta Force is installed. Vanguard and ACE will block the games until you run `bcdedit /set testsigning off` and reboot.

---

## 中文

### 结论

| 使用场景 | 签名方式 | 反作弊兼容性 |
|---|---|---|
| 开发调试 | 测试签名 (test signing) | 不兼容（Vanguard 和 ACE 会拦截测试模式） |
| 日常游戏（无畏契约、三角洲行动、CS2） | **Attestation 签名** | 兼容 |

建议：玩游戏的机器用 attestation 签名。测试签名只用于另一台开发机或虚拟机。

### 为什么测试签名不能用于游戏

测试签名需要执行 `bcdedit /set testsigning on` 并重启。测试模式下 Windows 会加载任意自签名证书签署的驱动。反作弊系统会检测这个状态：

- **无畏契约（Vanguard，内核驱动 vgk.sys）**：Vanguard 检查安全启动和测试模式。测试模式开启时 Vanguard 拒绝启动，游戏报 VAN 系列错误。
- **三角洲行动（ACE 反作弊）**：ACE 同样加载内核驱动，拒绝测试签名模式的系统。
- **CS2（VAC）**：VAC 是用户态反作弊，不拦截测试模式。但前两个游戏已经决定了结论。

### Attestation 签名（推荐）

Attestation 签名是微软硬件开发者中心的服务。你提交驱动包，微软联合签名后返回。签名结果可以在开启安全启动、关闭测试模式的 Windows 10/11 x64 上正常加载。

所需条件：

1. 微软合作伙伴中心（硬件）账户，注册免费。
2. EV 代码签名证书（DigiCert、Sectigo、GlobalSign 等），价格约每年 300–500 美元，证书存储在硬件令牌中。
3. 用 EV 证书给驱动包签名。
4. 提交到合作伙伴中心的 attestation 门户，微软通常几小时内返回联合签名的驱动包。
5. Attestation 签名的驱动不能使用仅供测试的功能。本驱动是普通音频驱动，不受影响。

具体步骤见 `scripts/attestation-sign.ps1`（需要浏览器操作的步骤在注释中说明）。

### 测试签名（仅开发用）

`scripts/install-driver.ps1` 自动完成测试签名安装：

1. 创建自签名代码签名证书（`SoundRadarTestCert`）。
2. 导入到本地计算机的根证书和受信任发布者存储区。
3. 给 `TabletAudioSample.sys` 和目录文件签名。
4. 开启测试模式（`bcdedit /set testsigning on`）。
5. 用 `pnputil` 安装驱动。
6. 提示重启。

`scripts/uninstall-driver.ps1` 卸载设备、驱动包和证书，并可关闭测试模式。

警告：不要在安装了无畏契约或三角洲行动的电脑上开启测试模式。Vanguard 和 ACE 会拦截游戏，直到你执行 `bcdedit /set testsigning off` 并重启。
