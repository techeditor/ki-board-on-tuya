# 版本规范 — Agentic Keyboard for Kiro

> 状态：**生效**（2026-09-30 起）
>
> 适用范围：项目版本、硬件版本、固件版本、Companion 版本。
>
> 目的：此前项目/硬件/固件/Companion 各用一套版本号（PRD v1.3、硬件丝印 v1.2.0、
> 固件 0.1.12、Companion 0.1.0），彼此不对齐，难以对应同一台实物。本规范统一到
> **单一项目主线号 `v0.x`**，各子系统在其下用 `v0.x.y` 派生。

---

## 1. 总原则

- **项目版本优先级最高**。正式量产 / 方案固化前，主线号固定在 `v0`（即 `v0.x`）。
  只有在方案固化、准备量产时才升到 `v1.0`。
- **硬件、固件、Companion 版本都遵循项目版本号**，统一格式 `v0.x.y`：
  - `x` = 项目主线迭代号（与项目版本一致）。
  - `y` = 该子系统在此主线内的**小版本更新 / 编译流水记录**（bugfix、小改、构建序号）。
- 同一台可工作的实物，其硬件 / 固件 / Companion 的 `x` **应当一致**（同属一个项目迭代）；
  `y` 各自独立递增，互不影响。

---

## 2. 项目版本主线（`v0.x`）

| 项目版本 | 方案 | 说明 |
|---|---|---|
| **v0.1** | 原始 Ki-Board 参考方案（外部作者） | 基于 **ESP32**、多块 SPI 屏的原始参考方案（见 `ref_projects/ki-board/`）。本项目的灵感来源。 |
| **v0.2** | 上一版（作者自研，基于 T5AI-Core） | 首个基于 **Tuya T5AI-Core** 的自研版本：5 机械键 + 5 向摇杆 + 18P LCD + 扩展板 USB(CH340C)。硬件实物 PCB 丝印曾标 `v1.2.0`（旧号，等价于本规范 **v0.2**）。 |
| **v0.3** | 当前迭代 | 最简化重构：去扩展板 USB/CH340C/电源开关；6 键（K1–K6）热插拔轴座；EC11 编码器替代 5 向摇杆；7× WS2812（K1–K6各1颗，LED7用于EC11）；12P LCD（ST7789P3）替代18P。详见 `hardware/pinmap/README-v0.3.md`。 |

> 历史号对照：**旧硬件丝印 `v1.2.0` = 项目 `v0.2`（的硬件）**。v0.3 起硬件丝印改用新规范号。

---

## 3. 子系统版本规则（`v0.x.y`）

### 3.1 硬件（PCB / 扩展底板）

- 版本号 = `v0.x.y`，随项目主线。当前 = **v0.3.y**。
- **落地方式（决策 A）**：PCB 工程名、原理图标题栏、丝印版本号一律标 `v0.3.y`
  （例如 `v0.3.0`）。不再使用 `v1.x` 内部号。
- `y` 用于同一主线内的改板流水（布线修正、器件替换等）。
- 导出物命名沿用 `hardware/exports/` 现有约定，把版本字段写成 `v0.3.y`。

### 3.2 固件（TuyaOpen / `firmware/app`）

- 版本号 = `v0.x.y`，随项目主线。进入 v0.3 硬件后，固件基线号升到 **v0.3.y**。
- `firmware/app/VERSION` 存**裸版本串**（不带 `v` 前缀），即 `0.3.y`。
- `y` = 构建 / 发布流水号（与固件发布流水一致）。
- 说明：v0.2 期间固件曾独立走 `0.1.x`（如 `0.1.12`）；进入 v0.3 后对齐到 `0.3.y`。
- `y` 由 pre-commit 钩子自动递增：每个改了 `firmware/app` 实现文件的提交 +1（`firmware/app/tools/version.py auto-bump-staged`），
  同一个提交多次构建版本号不变。不要跳过钩子。
- **0.3.19 是第一个 v0.3 硬件固件版本号**。之前的号要这样理解：
  - `main` 上 `0981f19`～`0746013` 标的 `0.3.17` 实际是 **v0.2 硬件**固件（引脚仍是 v0.2），已归档为 `firmware/archive/v0.2`；
  - `feat/v0.3` 分支开发期用过 `0.3.0`～`0.3.18`，这些号对应的构建不在 main 上，不再使用；
  - v0.3 合入 main 后从 0.3.19 起，由钩子继续递增（合入完成时为 0.3.39）。
- v0.2 归档固件（`firmware/archive/v0.2/app`）走 `0.2.y`，归档时为 `0.2.17`（接在 `release/v0.2` 的 0.2.16 之后）。
  归档版不挂钩子，需要修时手动 `python3 tools/version.py bump patch`。

### 3.3 Companion（Tauri：`companion/package.json` + `companion/src-tauri/Cargo.toml`）

- 版本号 = `v0.x.y`，随项目主线。当前对齐到 **v0.3.y**。
- **`companion/VERSION` 是唯一来源**，用 `companion/tools/version.py set|bump` 同步 `package.json`、
  `companion/src-tauri/Cargo.toml`、`companion/src-tauri/tauri.conf.json`；`version.py check` 校验一致。lock 文件随之更新。
- v0.3 合入 main 时统一为 `0.3.1`（main 之前是 0.1.0，`feat/v0.3` 是 0.3.0）。
- `y` = Companion 自身的小版本 / 构建序号。

---

## 4. 现状盘点与待同步项

进入 v0.3 时，以下位置的版本号需要按本规范对齐（具体改动在各自任务中执行，本表仅登记）：

| 位置 | 当前值 | 目标值 | 备注 |
|---|---|---|---|
| PRD（内部文档）头部「版本」 | v1.3 | v0.3.y | 同时更新日期与硬件描述（6键/EC11/12P LCD/无扩展板USB） |
| 硬件 PCB 工程 / 丝印 | v1.2.0（旧实物） | v0.3.0（新板） | 决策 A：改用项目号 |
| `hardware/exports/` 导出物命名 | v1.2 / v1.0 | v0.3.y | 新导出按新号 |
| `firmware/app/VERSION` | 0.3.y（已对齐，0.3.19 起） | 0.3.y | 2026-11-16 完成；2026-11-17 核对为 0.3.67 |
| `companion/package.json` version | 0.3.1（已对齐） | 0.3.y | 由 `companion/VERSION` 同步；2026-11-17 核对为 0.3.4（与 `companion/VERSION` 一致） |
| `companion/src-tauri/Cargo.toml` version | 0.3.1（已对齐） | 0.3.y | 由 `companion/VERSION` 同步；2026-11-17 核对为 0.3.4 |
| `README.md` / `README.zh-CN.md` 硬件表 | 5键/WS-1002/18P | 6键/EC11/12P | 已完成（2026-11-17 更新状态段） |

> 本文件只负责**定义规范**。实际改各文件的版本号，属于 v0.3 落地任务（见变更文档与后续同步任务），
> 避免在规范确立当天就大范围改动尚未定稿的内容。

---

## 5. 发版清单

每次发固件 / Companion 前记录并核对：

- [ ] `python3 firmware/app/tools/version.py check` 与 `python3 companion/tools/version.py check` 通过，记下两边版本号
- [ ] 记录构建所用的 **TuyaOpen SDK commit** 和 **T5AI 平台 commit**（`git -C <TuyaOpen> rev-parse HEAD`、`git -C <TuyaOpen>/platform/T5AI rev-parse HEAD`）
- [ ] 固件主机单测（`firmware/app/test/run_host_tests.sh`）、`check_pins_v0.3.py` 通过
- [ ] Companion `cargo test --lib`、`tsc --noEmit` 通过；Windows 上 `tauri build`
- [ ] 本次涉及引脚 / 灯链时序 / 电源时，附实板验证结果
- [ ] 要经 OTA 发布的固件：按 `docs/ota.md` §4 用 `AK_RELEASE_BUILD=1` 构建、离线签名、`verify` 复核；本版改过 KV 结构时确认能读旧值

v0.3 合入 main 时（2026-11-16）的基线：固件 0.3.39、Companion 0.3.1、v0.2 归档 0.2.17；
TuyaOpen `baedc51`，T5AI 平台 `e324ca0`。

---

## 6. 记录

- 2026-09-30：确立本规范。确认项目主线 v0.1（外部 Ki-Board 参考方案 / ESP32）、v0.2（自研 T5AI，旧丝印 v1.2.0）、
  v0.3（当前重构）；硬件/固件/Companion 统一 `v0.x.y`；硬件落地采用决策 A（丝印直接用项目号）。
- 2026-11-16：v0.3 分批合入 main。v0.2 打 tag `v0.2-final`（`0746013`）并归档到 `firmware/archive/v0.2`（0.2.17）；
  第一个 v0.3 硬件固件版本号为 0.3.19，之后由钩子递增；Companion 统一到 0.3.1，以 `companion/VERSION` 为准。
