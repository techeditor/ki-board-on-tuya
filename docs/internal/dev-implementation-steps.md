# 固件 + Companion 开发实施步骤（EC2/Runner 编译 · 本机烧录调试）

> 状态：实施蓝图（阶段 1 有线 MVP 为主，含阶段 2/3 衔接）。中文优先。
> 适配约束：**本机磁盘/算力有限** → 重编译放 **EC2 + GitLab Runner**；
> **本机只负责固件烧录、串口调试、Companion 运行与手测**。
> 基线现状（2026-08-25 核对）：
> - `firmware/`：产品 app 已起步（`firmware/app/`，机制 A = 单 app + 共享核心 + per-variant config；
>   Task 3 骨架 = v1.2 板级配置(WS2812→P4/SPI1, LCD_BL→P8) + UART0 `hello_ack`/心跳，EC2 `tos.py build` 通过，
>   clang-format 门禁通过）；阶段 0.5 bring-up 已建立，
>   UART 回环、KEY1、全输入、ST7789、WS2812、Wi-Fi/BLE 共 6 个独立固件
>   均使用项目 IO 并已构建成功，等待 Windows 实机按统一测试计划验收。
> - TuyaOpen：`/data/projects/TuyaOpen` 已完成 `tos.py check`，子模块、T5AI 平台
>   (`52c6e54b`) 与 GCC Arm 10.3 工具链已就绪；`ref_projects/signal_light_demo`
>   使用 `TUYA_T5AI_CORE.config` 全量构建成功。
> - `companion/`：Tauri 骨架完成；Node 22.23.2 / Rust 1.98.0 已锁定，
>   `package-lock.json` 已生成；Rust 4 个单测、TypeScript typecheck、Vite build 均通过。
> - `shared/protocol/protocol.md`：协议 v1 已定稿。
> - **无 CI**（无 `.gitlab-ci.yml`）。仓库托管于 GitLab（`gitlab.aws.dev`）。
> 任务编号沿用 `docs/internal/implementation-plan.md` 的 16 任务（Task 3–16）。

---

## 0. 分工模型（先读这一节）

```
┌───────────── EC2 (Linux, 编译主力) ─────────────┐     ┌───── 本机 Windows (烧录/调试/运行) ─────┐
│ • TuyaOpen SDK + tos.py 全量工具链              │     │ • 默认无编译工具链(省空间;可按需临时装)  │
│ • 固件 tos.py build → dist/<version>/ QIO/UG bin │ scp │ • CH343SER 驱动 + tyutool/WebTool 烧录    │
│ • 固件 host 侧单测(input_fsm/protocol)           │────▶│ • 串口看日志(460800)                      │
│ • (可选)Companion Linux 侧 cargo test/clippy     │     │ • 运行 Companion(Windows 安装包)          │
└──────────────────────────────────────────────────┘     │ • 手测: 按键→注入 / 语音 / LCD / hook     │
┌───────── GitLab Runner (CI, 产物工厂) ──────────┐     └────────────────────────────────────────┘
│ • firmware job: build → 上传 bin 为 artifact      │
│ • companion job: Windows Runner 或交叉编译         │     产物经 GitLab artifacts / Release 分发,
│   → tauri build 产 NSIS 安装包(.exe/.msi)          │     本机点下载即可,不在本机编译。
│ • 单测 job: cargo test + tsc + 固件 host 单测      │
└────────────────────────────────────────────────────┘
```

**三条铁律（默认原则，非绝对）**
1. **默认在 EC2/远端编译**：本机通常不装 TuyaOpen 工具链、不跑 `tauri build`（磁盘/时间成本高，见 PRD 风险 R3）。
   ⚠️ **例外**：少数情况会在本机编译——例如 Companion 本机调试（D3 已定先本机编译）、临时快速验证、
   离线无法连 EC2 时。届时本机需临时具备对应工具链（Node/Rust 或 TuyaOpen），编译产物（`target/`、`.build/`、`*.bin`）
   已被 `.gitignore` 忽略、不入库。**原则是"默认远端、按需本机"，不是"本机绝不编译"。**
2. **EC2 看不到本机 USB 串口** → 烧录/monitor 必须在本机做（除非上 usbipd 反向隧道，非必要不上）。
3. **产物流向单一**：编译在云端 → bin/安装包经 scp 或 GitLab artifacts 落到本机 → 本机烧录/运行。

---

## 1. 前置准备（一次性）

### 1.1 EC2（固件与 CI 编译主力）
- [ ] 规格建议：**≥ 4 vCPU / 8–16GB RAM / 40GB+ 磁盘**（TuyaOpen 全量 SDK + 编译缓存约 5–10GB；Rust target 3–5GB）。
- [ ] OS：Ubuntu 22.04 LTS（TuyaOpen 官方验证度高）。
- [ ] 安装 TuyaOpen：`git clone` TuyaOpen → `. export.sh` → `tos.py` 可用（依赖见 KB `03-toolchain-setup/00-environment-setup.md`）。
- [ ] 拉本仓库：`git clone <gitlab repo>`（配好 SSH key / LFS）。
- [ ] 验证：`cd firmware && tos.py config choice`（能列出并选 `TUYA_T5AI_CORE.config`）。

### 1.2 本机 Windows（烧录/调试/运行）
- [ ] 装 **WCH CH343SER** 驱动（板载 CH342F 双通道）→ 设备管理器出现 2 个 COM（如 COM5/COM6）。
- [ ] 烧录工具二选一：**WebTool**（https://tuyaopen.ai/web-serial ，Chrome/Edge，零安装）或 **tyutool GUI/CLI**（https://tuyaopen.ai/tyutool ）。
- [ ] 装 **OpenSSH client**（Win10/11 自带）用于 `scp` 拉 bin。
- [ ] （运行 Companion 需要）Windows 10/11 x64；后续装 CI 产出的安装包即可，**无需 Rust/Node 环境**。

### 1.3 GitLab Runner（CI）
- [ ] 固件 build job：Linux Runner（docker executor 或 EC2 shell runner，复用 1.1 的 TuyaOpen 环境）。
- [ ] Companion build job：**Windows Runner**（产 Windows 安装包最稳）或 Linux + 交叉编译（见 §4 决策点 D3）。
- [ ] 配置 CI 变量：SSH key、（如需）签名证书、Tuya License 等 **走 CI Secret，不入库**（PRD C6）。

---

## 1.5 阶段 0.5 · 硬件测试程序（Bring-up，先于正式 Task）

> 目的：用一系列**独立、最小**的固件测试程序，逐个验证 PCB v1.0 的焊接/接线正确，
> 在写业务逻辑前排除硬件问题。每个测试程序独立可编、可烧、可观测。
> 编译在 EC2，烧录/观测在本机（`scripts/flash.ps1`）。
> 本项目 bring-up 位于 `firmware/bringup/`，统一构建入口为 `build_all.sh`，
> 实机验收顺序见 `TEST_PLAN.md`。`signal_light_demo` 只作代码结构和编译链路参考，
> 不直接用于扩展底板验收（其 IO 与本项目 LCD 引脚冲突）。

### 测试序列（按依赖/风险递进）

| # | 测试程序 | 验证的硬件 | EDA 依据 | 通过判据 |
|---|---------|-----------|---------|---------|
| **T0** | **编译+烧录闭环**（`key1_uart`） | CH340C(U3) 烧录链路 + UART0(P10/P11) + 电源 | netlist: UART0/CH340C/自动复位 | tyutool 烧录成功；UART0 115200 收到 ready/heartbeat JSONL |
| **T1** | **串口心跳 + KEY1**（`key1_uart`） | UART0 收发、CH340C 通信、KEY1=P24 | R5(TX隔离)、Q2(复位)、K1 | PC 周期收到心跳；按下/松开 KEY1 收到 down/up JSONL |
| **T2** | **SPI-LCD 点屏**（ST7789） | LCD SPI(P14/P16/P15/P17)、RST(P6)、BL(P8, v1.2 由 P5 迁入)、PWR(P7)、FPC1 | pinmap §3 18P FPC、R11/R12 阻尼、背光 R1/Q1 | 屏亮、填色/色带/文字正常，无花屏；背光可控 |
| **T3** | **按键扫描**（5× 茶轴 K1-5） | KEY1-5 = P24/P25/P26/P27/P23，内部上拉低有效 | pinmap §4、K1-5 | 每键按下/抬起串口打印正确、无抖动重复 |
| **T4** | **摇杆扫描**（5 向 SW2） | NAV = P32/P20/P21/P22/P30（UP/DOWN/LEFT/RIGHT/CENTER） | pinmap §4、SW2(WS-1002)、U4 扩展焊盘 | 5 方向识别正确、公共端 GND OK、无串扰 |
| **T5** | **WS2812 灯效**（3 颗菊花链） | P4(SPI1_MOSI, v1.2 由 P13 迁入) → U2(SN74LV1T34 电平转换) → LED1-3(5V)、R3 串阻 | netlist WS2812 段、U2、R3 | 3 颗独立可控 RGB、颜色/顺序正确（验证电平转换有效） |
| **T6** | **电源指示灯**（LED4/LED5） | LED4=VBUS 指示(R6)、LED5=电池指示(R10) | netlist 指示灯段 | 插 Type-C 亮 LED4；电池供电亮 LED5（纯硬件，肉眼验证，无需固件） |
| **T7**（可选） | **电池/开关/充电** | BT1 电池座、SW1(SK12D07VG5) 电池总开关、并联 BAT 网 | PRD §3.4/§3.6 | 开关通断电池路径正确；Type-C 插入系统仍供电；⚠️ 只插一节电池 |
| **T8**（阶段2预备） | **Wi-Fi/BLE 自检** | T5 模组射频 | — | 扫到 AP / BLE 广播可见（为配网/BLE 传输铺路） |

### 每个测试程序的产物形态
- 独立 TuyaOpen app，避免相互干扰；**每个都能单独 `tos.py build` 出 bin**。
- 目录：`firmware/bringup/<name>/`（EC2 编译；源码入库，bin 不入库）；
  公共 IO 真值在 `firmware/bringup/common/include/agentic_board_io.h`。
- 观测方式：串口日志（T1/T3/T4）、肉眼（T2/T5/T6/T7）、抓包（T8）。

> **实现状态**：T0/T1 为 `key1_uart`（另有 `uart_loopback`），T2 为
> `lcd_st7789`，T3/T4 合并为 `input_scan`，T5 为 `ws2812`，T8 为
> `wireless_selftest`；T6/T7 是纯硬件手测。固件统一构建已通过，实板验收待执行。

---

## 2. 固件侧实施步骤（Task 3–11 · 阶段 1 有线）

> 编译在 EC2；每步产物 bin 拉到本机烧录 + 串口验证。固件 host 侧逻辑（状态机/协议）单测在 EC2/CI 跑。

### Task 3 · 固件可编译烧录（心跳 + 板级 Config）
**EC2：**
1. 在 `firmware/` 基于 `TUYA_T5AI_CORE.config` 建最小 app（app 入口 + 串口打印心跳）。
2. 落地板级引脚宏（对齐 PRD §7.1 / `hardware/pinmap/`）：LCD SPI(P14/P16/P15/P17/P6/P8/P7，v1.2 背光 P5→P8)、
   KEY1-5(P24/P25/P26/P27/P23)、NAV(P32/P20/P21/P22/P30)、WS2812_DIN(P4=SPI1_MOSI，v1.2 由 P13 迁入)；分辨率 320×240。
3. `tos.py clean -f && tos.py build` → 产 `dist/<version>/*_QIO_*.bin`（旧版可能为 `.build/bin/`）。
**本机：**
4. `scp ec2:<path>/dist/<version>/*_QIO_*.bin .` → tyutool/WebTool 烧录（460800，**开电源开关**）。
5. **验证**：串口 monitor 见周期心跳输出。
✅ Demo：上电串口打印心跳。

### Task 4 · 输入扫描（按键 + 5 向摇杆）
1. 实现 GPIO 扫描 + 去抖(10–20ms) + 长按(默认 800ms)，产生 key/nav 的 down/up/long。
2. **状态机抽成 host 可测模块**（不依赖硬件）→ EC2/CI `host 单测`。
3. 本机：烧录后拨按键/摇杆，串口打印事件。
✅ Demo：按键/摇杆串口实时打印；单测通过。

### Task 5 · UART 事件上报（协议 v1）
1. 事件按 **JSONL** 经 UART 发出（`v/seq/type/payload`），seq 递增 + ack；传输层做成接口，UART 为首个后端。
2. host 侧协议编解码单测（对齐 `shared/protocol/protocol.md`）。
3. 本机：串口抓包见规范帧。
✅ Demo：操作输入 → 串口见规范 JSONL 帧。

### Task 9 · LCD UI（LVGL @ ST7789）
> （早于 Companion 也可，硬件独立）
1. 接 ST7789 + LVGL，显示连接态/按键提示/语音态，预留 Agent 状态区；显示逻辑与数据解耦（host 单测状态映射）。
2. 本机：烧录观察屏显。
✅ Demo：操作/语音时屏上实时反映；无花屏。

### Task 10（固件侧） · 接收 display 命令上屏
1. 固件解析 PC→板 `display`（agent_name/state/text）→ 更新 LCD Agent 区。
2. 与 Companion 联调（见 §3 Task 10）。
✅ Demo：PC 下发 display，LCD 实时显示 Agent 状态。

### 固件阶段 2/3 衔接（Task 12/15）
- Task 12：新增 `ble_gatt` 传输后端，复用协议 v1；与 UART 并存可切换。
- Task 15：BLE HID over GATT（NimBLE 手搓，绕过 TAL/TKL）；HID 管按键、GATT 管状态/语音。
- 编译仍在 EC2，本机烧录 + BLE 抓包验证。

**固件改-烧循环（本机 `flash.ps1`，可选）**
```powershell
param([string]$Port="COM5")
$remote="ec2-user@<EC2_IP>:<repo>/firmware/dist/<version>/"
scp "$remote*_QIO_*.bin" .\fw.bin
tyutool_cli write -d t5ai -p $Port -b 460800 -f .\fw.bin   # 参数以 --help 为准
```

---

## 3. Companion 侧实施步骤（Task 6–11 · Tauri Rust + React/TS）

> **编译在 Runner/EC2**（Linux 侧可跑 `cargo test`/`clippy`/`tsc`）；**Windows 安装包由 Windows Runner 产**；
> 本机**只装安装包运行 + 手测**。骨架已有 `protocol.rs`(3 单测) + `transport/` 骨架，直接续写。

### Task 6 · 串口后端与协议解析（Rust）
1. `transport/usb_serial.rs`：serialport 连 COM、行读取线程、VID/PID+product 自动发现端口。
2. 解析器与 IO 解耦、坏帧/截断/未知 type 容错（复用/扩展 `protocol.rs`）。
3. 事件经 Tauri event 推前端。对照 ki-board `usb_serial.rs`+`protocol.rs`。
4. **CI**：`cargo test`（字节流回放 + 坏帧用例）。
✅ Demo：板操作 → Tauri 窗口实时显示事件。

### Task 7 · 键盘注入到焦点窗口
1. key/nav/macro 按可配置映射(JSON) → Win32 **SendInput** 注入；长文本 **arboard** 粘贴。
2. 注入层封装可 mock 断言 INPUT 序列。
3. **注意**：SendInput 是 Windows API → 此模块**必须 Windows 目标**编译（交叉编译需 `x86_64-pc-windows-msvc/gnu`，见 D3）。
✅ Demo：Kiro 终端聚焦下按板键触发预设命令。

### Task 8 · 语音触发 → Windows 系统 ASR → 注入
1. 板发 `voice:start` → Companion 调 **Windows.Media.SpeechRecognition**（WinRT，`windows` crate，用 PC 麦克风）听写 → 注入。
2. 先写独立探针 example 验证 WinRT 链路；处理无结果/超时/权限。
3. ASR 适配层可 mock 单测。
✅ Demo：聚焦 Kiro 说话，识别文本自动出现。

### Task 10（Companion 侧） · Kiro CLI hook → Agent 状态
1. hook 脚本把 agentSpawn/userPromptSubmit/stop/postToolUse 转 JSONL → 交 Companion → 下发板 `display`。
2. Companion 不可用时 hook 回退直连串口。
✅ Demo：Kiro 跑 agent，LCD 实时显示状态。

### Task 11 · 消弹窗 + 打包
1. 纳入 `powershell_shim`（Rust GUI 子系统）消 hook 黑框；提供 `kiro-silent.bat`。
2. **`tauri build` 在 Windows Runner 产 NSIS 安装包**（.exe/.msi）→ GitLab artifacts/Release。
3. 本机下载安装包运行验证。
✅ Demo：完整有线闭环（按键/语音/Agent 状态全通、无弹窗、安装包可分发）。

### Companion 阶段 2（Task 13）
- `transport/ble_gatt.rs`：btleplug 扫描/连接/通知，与串口后端统一接口；`ble_probe` example 先验证。

---

## 4. CI/CD 设计（`.gitlab-ci.yml`）— **当前暂缓，待流程顺后启用**

> 依 D2/D3/D8 结论：**当前不上 CI**。固件**手动在 EC2 `tos.py build`**（见 §2/§9），
> Companion **本机编译**（见 §3）。以下为**将来**要自动化时的设计，先留档。

**将来 stages**：`test` → `build` → `package` → `release`

| job | runner | 作用 | 产物 |
|-----|--------|------|------|
| `companion-test` | Linux | `cargo test`(protocol/transport) + `tsc`/前端构建 | 测试报告 |
| `companion-clippy` | Linux | `cargo clippy -D warnings` | — |
| `firmware-build` | Linux(TuyaOpen 环境) | `tos.py clean -f && tos.py build` | `*_QIO_*.bin` / `*_UG_*.bin` (artifact) |
| `firmware-host-test` | Linux | 固件 host 侧逻辑单测(input_fsm/protocol) | 测试报告 |
| `companion-package` | **Windows** | `tauri build` → NSIS 安装包 | `.exe`/`.msi` (artifact) |
| `release` | any | 打 tag 时把 bin + 安装包挂 GitLab Release | Release 附件 |

**要点**
- 固件 build 依赖 EC2/Runner 上已就绪的 TuyaOpen（用自定义 docker image 或 shell runner 复用 §1.1 环境，避免每次重装 SDK）。
- Companion Windows 包**优先用 Windows Runner**（SendInput/WinRT/NSIS 原生最稳）；无 Windows Runner 时走交叉编译（D3）。
- artifacts 设 `expire_in` + 只在 tag/主分支上传 Release，控制存储。
- 缓存：Rust `target/` 与 cargo registry、TuyaOpen build 缓存做 CI cache，加速。
- **Secrets 走 CI 变量**（SSH、证书、Tuya License），不入库。

**骨架（占位，待 D 决策后落地）**
```yaml
stages: [test, build, package, release]

companion-test:
  stage: test
  tags: [linux]
  script:
    - cd companion && npm ci && npm run build
    - cd src-tauri && cargo test
    - cargo clippy -- -D warnings

firmware-build:
  stage: build
  tags: [tuyaopen]         # 复用装好 TuyaOpen 的 runner
  script:
    - cd firmware
    - . <TuyaOpen>/export.sh
    - tos.py clean -f && tos.py build
  artifacts:
    paths: [firmware/dist/**/*_QIO_*.bin, firmware/dist/**/*_UG_*.bin]
    expire_in: 2 weeks

companion-package:
  stage: package
  tags: [windows]          # Windows runner 产 NSIS 安装包
  script:
    - cd companion && npm ci
    - npm run tauri build
  artifacts:
    paths: [companion/src-tauri/target/release/bundle/**]
    expire_in: 2 weeks
  rules:
    - if: '$CI_COMMIT_TAG'
```

---

## 5. 决策点结论（已与开发者确认，2026-08-25）

| # | 决策 | **结论** |
|---|------|---------|
| **D1** | EC2 规格与常驻方式 | ✅ **EC2 已就绪、常驻**。devbox = `ip-172-31-43-51`（Amazon Linux 2023）；`/data` 200G。TuyaOpen 在 `/data/projects/TuyaOpen`，`tos.py check`、T5AI 平台下载及 `signal_light_demo` 全量构建均已验证。项目 clone 在 `/data/projects/tuya-powered-agentic-keyboard-for-kiro`。 |
| **D2** | 固件 CI runner 形态 | ✅ **CI 暂缓**。含义澄清：D2 是"以后让 GitLab CI 自动编固件时，Runner 从哪来、怎么拿到 TuyaOpen 环境"。两种形态：(a) 把常驻 EC2 注册为 **shell runner**（复用已装 TuyaOpen，最省）；(b) 做含 TuyaOpen 的 **docker image**。**当前先手动在 EC2 `tos.py build`**，不上 CI；要上时选 (a)。 |
| **D3** | Companion 编译 | ✅ **先本机编译**（不用 Linux 交叉编译；GitLab Runner 暂不试）。磁盘由开发者人工确认。Companion 是 Windows GUI，本机编译最便于调 SendInput/WinRT/麦克风。 |
| **D4** | 本机取固件产物方式 | ✅ **scp 拉 bin + 一键烧录脚本**（`scripts/flash.ps1`）。 |
| **D5** | usbipd 串口透传 | ✅ **默认本机烧录**；**可尝试 usbipd-win 透传**（把本机串口透到 EC2，实现 EC2 端 `tos.py flash/monitor` 一条龙）作为进阶选项，见 §9。 |
| **D6** | 凭据管理 | ✅ **本地自行管理，不入库**（Tuya UUID/AuthKey、ASR key）。`.gitignore` 已含密钥模式。 |
| **D7** | 开发顺序 | ✅ **先做一系列硬件测试程序**（SPI-LCD、按键/摇杆传输等）逐项验证 PCB v1.0，再进正式 Task。见新增 **§1.5 阶段 0.5**。 |
| **D8** | GitLab Release 触发 | ✅ **暂缓**。含义澄清：Release = 给某 commit 打版本 tag，把编译产物(bin/安装包)作为附件挂在 GitLab Release 页做"正式版本快照"。触发方式：(a) 打 tag 自动 / (b) 手动。还涉及 artifacts `expire_in`(中间产物过期清理)。**等阶段 1 有可分发安装包后再启用**。 |

> **重大发现（2026-08-25）**：EC2 上已有参考项目 **`signal_light_demo`**
> （`/data/projects/tuya-powered-agentic-keyboard-for-kiro/ref_projects/signal_light_demo`）——同为 T5AI-Core 的
> "AI 编程助手键盘"（语音听写→UART 注入 PC + 状态灯 + OLED，联动 Claude Code/Codex），标准 TuyaOpen app
> 结构，已含 `config/TUYA_T5AI_CORE.config`，`cd source/embedded && tos.py build` 可编。模块(signal_transport/
> voice_key/led/agent_status/ws/oled)几乎就是本项目蓝图。**作为固件起点参考**：先编它跑通"EC2 编译→本机烧录"闭环，
> 再做本项目差异化（5 键+摇杆输入、ST7789 LCD、Tauri Companion、Kiro hook）。

---

## 6. 里程碑与验收（阶段 1 有线 MVP）

| 里程碑 | 内容 | 验收（Demo） |
|--------|------|-------------|
| M1 | 固件心跳可烧录 (Task3) | 本机烧录后串口见心跳 |
| M2 | 输入 + UART 协议 (Task4-5) | 操作 → 串口规范 JSONL；host 单测过 |
| M3 | Companion 串口 + 注入 (Task6-7) | 板键 → Kiro 窗口产生输入 |
| M4 | 语音 ASR (Task8) | 说话 → 文本注入 Kiro |
| M5 | LCD + Agent 状态 (Task9-10) | LCD 实时显示 Agent 状态 |
| M6 | 打包 + 无弹窗 (Task11) | 安装包可分发；hook 无黑框；**完整有线闭环** |

> CI 门禁：每个 Task 完成跑对应 job（`cargo test`/`tsc`/固件 host 单测/`tos.py build`），绿灯才并入。

---

## 7. 关键风险与规避

| 风险 | 规避 |
|------|------|
| 本机磁盘不足跑不动 Tauri 全量编译 (R3) | 编译全放 EC2/Runner，本机只运行安装包 |
| EC2 看不到本机 USB 串口 | 烧录/monitor 固定在本机；不强求 EC2 一条龙 |
| WinRT/SendInput 交叉编译坑 | 用 Windows Runner 原生编译（D3-a） |
| TuyaOpen SDK 每次 CI 重装慢 | shell runner 复用环境 or 预装 docker image（D2） |
| 固件改 .config 未 clean 导致编译异常 | CI 固定 `tos.py clean -f && tos.py build` |
| 凭据泄漏 | CI Secret + keyring，`.gitignore` 已含密钥模式 |
| 产物版本混乱 | bin/安装包统一走 GitLab Release + 版本 tag |

---

## 8. 下一步（已确认路线）
1. ✅ 决策点 D1–D8 已确认（见 §5）；EC2/TuyaOpen/tos.py 已验证可用。
2. ✅ **EC2 编译已打通**：`ref_projects/signal_light_demo` 已生成
   `dist/embedded_1.0.9/embedded_QIO_1.0.9.bin`。
3. **完成本机烧录闭环（T0/T1）**：运行已修正路径与产物搜索逻辑的
   `scripts/flash.ps1`，验证 scp + tyutool 烧录，并以 UART0 115200 观察
   ready/heartbeat 与 KEY1 down/up JSONL。
4. **逐项做硬件测试程序**（§1.5）：T2 SPI-LCD（开发者起草）→ T3 按键 → T4 摇杆 → T5 WS2812 → T6 指示灯。
5. 硬件验证 OK 后进正式 **固件 Task3-5**，并行 **Companion Task6-8**（本机编译）。
6. CI/Release 待流程顺后再启用（§4，Runner 选 D2-a）。

> 参考：`docs/internal/implementation-plan.md`(16 任务)、`docs/prd.md`(§7 软件规格/风险)、
> `shared/protocol/protocol.md`(协议 v1)、`tuya-kb/03-toolchain-setup/01-cloud-build-local-flash-windows.md`(云编译本地烧录)、
> `ref_projects/ki-board/`(transport/protocol/ASR 可对照复用)。
