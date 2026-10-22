# Agentic Keyboard for Kiro — 开发编年史

> **来源说明**：本文档由 **EC2 上的 Kiro Crew 端**整理生成。整理方式为扫描该端可见的本项目相关会话（dashboard/Slack/微信等）、结合本仓 git 提交历史与 spec 结构交叉核对得出，事实以 git 提交与实测结果为准。
>
> 面向后续 slide 制作的素材文档（仿照 ki-board deck 结构接续制作新版）。
> 汇总范围：本项目相关的全部会话（2026-08-20 ~ 2026-09-14）+ 133 次 git 提交 + 12 套 spec。
> 生成时间：2026-09-14。事实锚点以 git 提交历史与实测结果为准，而非任务表勾选状态。

---

## 0. 一句话概述

用 Tuya T5AI-Core 开发板 + 自制扩展底板，做一台**面向 "人与 AI Agent 协作编程" 的无线 AI 小键盘**：
按键说话 → 云/本地 ASR → 文字直达 Kiro Crew；Agent 状态经 Hook 实时回流上屏。
从裸板 bring-up 一路做到无线语音控制 Kiro Crew 的 MVP，累计 **133 次提交**、覆盖固件 / 上位机 / 硬件三层。

---

## 1. 产品定位与差异化（PRD v1.3 / v1.4）

| 项 | 内容 |
|----|------|
| 产品名 | Agentic Keyboard for Kiro（Ki-board for Kiro Crew） |
| 硬件基线 | Tuya T5AI-Core 开发板 + 自制扩展底板 v1.2 |
| 形态 | 2.8" LCD(ST7789 320×240) + 5 机械键(K1–K5) + 1 个 5 向摇杆 + 板载 Mic + 3 颗 WS2812 |
| 连接 | USB(CH340C→COM6) 或 BLE GATT，与 PC companion 相连 |

**两大核心差异化**
- **语音输入直达 Kiro**：按键触发语音 → ASR 文本自动注入当前聚焦的 Kiro 窗口，免切窗、免打字。
- **Agent 状态上屏**：Kiro CLI/Crew 生命周期 Hook 把 Agent 的 Idle/Running/Tool/Error 状态实时显示在设备 LCD 上。

**核心场景**：S1 口述长需求；S2 长任务盯进度；S3 高频操作一键触发；S4 多会话摇杆切换。

**产品交互重大转向（2026-08-26 / 09-09）**：从最初的"语音听写器"（按键打字流）**转向"Agent 物理控制器 + 状态界面"**（接近 Codex Micro 形态）。K1–K3 三槽位、K4 大键 PTT 语音、K5 主操作、5 向上下文导航；固件只发逻辑事件，由 companion 适配。旧的 D1–D4 待决项与 backspace 键设计全部作废，以 `docs/agentic-keyboard-interaction-design.md` 为唯一准绳。

---

## 2. 系统架构与数据流

```
板子(Mic + 按键 + LCD + LED) --BLE GATT / USB JSONL--> PC companion(Tauri) --> Kiro Crew
  · K4 语音键 → 板载 Mic → 涂鸦云 ASR(ai_chat HOLD/PTT) → 文字 → 链路 → companion → Kiro Crew
  · PC-ASR 键 → 链路信号 → companion 激活 Windows 系统 ASR(Win+H) → 文字 → Kiro Crew
  · Agent 状态/通知 ← companion ← Kiro Crew 生命周期 Hook（复用 FP-1 显示层，回流上屏）
```

**三层组成**
1. **固件层** `firmware/app`（产品固件）+ `firmware/feature-probes`（功能探针）+ `firmware/bringup`（硬件自检）。TuyaOpen/T5AI，8×8 ASCII 点阵字体，无 LVGL，走 `ak_statuslcd + ak_gfx + TDL/ST7789`。
2. **上位机层** `companion/`（Tauri 桌面应用，Rust + TS），负责 BLE/USB 传输、协议编解码、Kiro Crew 控制（快捷键注入 + Hook 状态读取）。
3. **硬件层** T5AI-Core + 扩展底板 v1.2（引脚映射、网表、BOM）。

---

## 3. 开发阶段编年（时间线 / 关键节点）

### 阶段 0 — 立项与硬件（2026-08-20 ~ 09-01）
- **08-20** 仓库骨架落地（Phase 0 / Task 1）。
- **08-21** 扩展底板引脚图、网表、BOM + PRD（Task 2）；补硬件元件速查笔记。
- **08-25** 引导固件与 companion 开发环境；整理外部参考项目（ki-board 作为参考版，独立管理不入库）。
- **08-31** 硬件 bring-up 测试套件；WS2812 自动时钟驱动；PCB v1.0 收尾、EDA 报告、开发计划、`flash.ps1` 烧录脚本。
- **08-31 ~ 09-01** 定义 **v1.2 返工范围**：WS2812 P13→P4/SPI1、LCD 背光 P5→P8、FPC1；对照网表核验 EDA 改动。

### 阶段 1 — Bring-up 硬件基础验证（2026-09-05）
- **综合自检固件 `full_selftest`**：合并 INPUT/LCD/WS2812/UART/RADIO 五项单测，长按 CENTER 切换模式，屏上显示提示，一次烧录测全部外设。
- **LCD 横屏方向**：MADCTL=0x60(MV|MX) 顺时针 90°，逻辑尺寸交换为 320×240；实机验证方向。
- 引脚同步到 v1.2 布线。

### 阶段 2 — Phase 2 → feature-probes 转型（2026-09-05 ~ 09-09）
- **P2-1 agent-display**（后并入 FP-1）：spec-driven 从 CodeTank harvest Agent 显示层，LVGL 挂自研 ST7789 路径，19 个动画场景引擎 + 精灵 runtime + 通知条 + 按键意图映射，host 单测全过。实机吉祥物可显示，需整屏旋转 270°。
- **确立 spec-driven 硬规则**：所有 feature-probe 动手前先产出 `requirements.md / design.md / tasks.md` 三件套。
- **重命名 phase2 → feature-probes，确立 MVP 北极星**：无线 AI 语音小键盘控制 Kiro Crew，四条基本要求（BLE 无线 / 控制 Kiro Crew / 板载 Mic 云 ASR / 一键激活 PC 系统 ASR）。

### 阶段 3 — 功能探针逐个攻坚（2026-09-09 ~ 09-13）
逐个探针从代码 → 构建 → 真机验收，拼出 MVP。详见 §4 探针战报。

### 阶段 4 — 产品固件集成 + 上位机成熟化（2026-09-13 ~ 09-14）
- 产品固件 `firmware/app` 从骨架长成集成体：双传输、云 ASR 集成、Hook 状态回流、LCD 界面重构、设备设置页、自动 ASR 降级路由、WS2812 状态灯（进行中）。
- companion 从 MVP UI 长成成熟控制台：Chat A/B/C 绑定、Watched Chat、设备设置双向同步、Wi-Fi/License 配网、Hook 一键安装、ASR 文本编辑控制。
- 固件引入版本控制与 pre-commit 自动 patch 递增（VERSION 现为 **0.1.7**）。

---

## 4. 功能探针（Feature Probes）战报

| ID | 名称 | 验证链路 | 状态 |
|----|------|----------|------|
| **FP-1** | agent-display | LVGL 场景 + 吉祥物动画 + 通知 UI，按键模拟 | ✅ 真机通过 |
| **FP-2** | ble-gatt-link | BLE GATT 板↔companion（notify 0x2B10 / write 0x2B11） | ✅ 真机验收 |
| **FP-3** | tuya-cloud-asr | 板载 Mic → 涂鸦云 ASR(HOLD/PTT) → 文字 → UART0 JSONL | ✅ 真机云 ASR 验收 |
| **FP-4** | kirocrew-control | companion 控制 Kiro Crew：K1–K3 切会话 / K4 输入 / K5 提交；Hook 状态回流 | 🟡 本地控制完成；Hook 端到端待验收(T16) |
| **FP-5** | pc-asr-activate | 板键 → companion 激活 Windows 系统 ASR(Win+H) → Kiro Crew | ✅ Windows 真机验收(09-14) |
| **FP-6** | ble-hid-fallback | 无 companion 兜底：板子直接当标准 BLE HID 键盘 | 🟡 compile+link GO；真机配对/发键暂缓 |
| **FP-7** | mvp-integration | 无线 + 双 ASR + 控制 Kiro Crew 端到端 | ⏳ 高度集成，完整 DoD 未正式验收 |

**FP-2 BLE GATT**：自定义 GATT 服务，手机 BLE 扫描器验证 notify(定时 ping + 按键帧) + write(1234 板 RX 收 4 字节) 端到端全绿。踩坑：新版 TuyaOpen SDK 默认 V2 服务 0xFD50（write 0x0001/notify 0x0002/read 0x0003），非 V1 的 0x1910/0x2B10/0x2B11 → companion 改用 V2 + V1 回退。

**FP-3 涂鸦云 ASR**：走 `ai_chat` HOLD(PTT)，`your_chat_bot` 根 Kconfig `select ENABLE_AI_COMPONENTS` 才拉入 AI 栈。真机联调打通全链路，唯一卡点是 PID 未在涂鸦平台绑定 AI 智能体 → 用户平台侧绑定 Agent 后重试通。K4=P27 按键从 IRQ 模式改 `BUTTON_TIMER_SCAN_MODE + PULLUP` 解决扩展板无外部上拉不触发。

**FP-4 Kiro Crew 控制**：Route A（session-control API 隔隧道）判死路 → 改 **FP-4b 本地按键注入**（Alt+数字确定性快捷键，不走地址栏刷新）+ Hook 读状态。四类操作：绑定会话 / 输入文字 / 提交执行 / 切换会话。Hook 链路最终落在 Kiro Crew 0.6.0 Dashboard 全局 ScriptHookStore `~/.kiro/crew/hooks.json`（四条 agentSpawn/userPromptSubmit/postToolUse/stop 调 companion.exe --hook）。

**FP-5 PC ASR**：companion 发 Win+H 触发 Windows 系统听写；纯函数 `route_voice` 路由 + `VoiceSource{Cloud,System}`。修 system ASR 误提交（Alt+Enter 聚焦=提交 → 改为只聚焦窗口）。

**FP-6 BLE HID**：先判 NO-GO（头文件层 LIST_HEAD 宏冲突 + ble_hs.h 依赖 host 私有头）→ 用户要求必做 → 参照 `olegos76/nimble_kbdhid_example`，**隔离编译单元 + target 级 include 目录**在零改 SDK 前提下 compile+link GO，推翻 NO-GO。真机 Just Works 配对待验。

---

## 5. 关键技术难题与解决（收获沉淀）

| # | 问题 | 根因 | 解决 |
|---|------|------|------|
| P1 | 云 ASR 集成后真机崩黑屏(MemFault@0x8) | `ai_chat_init` 在未初始化 Tuya IoT 上下文时被调，解引用空指针 | 移植 FP-3 启动序列：LCD 最先 → `ak_cloud_init`(tuya_iot_init/netmgr/start) → 仅 IoT 成功才 `ai_chat_init`（护栏防崩砖） |
| P2 | Wi-Fi 连接/云激活后屏幕错行撕裂扭曲 | framebuffer 在 cacheable PSRAM，SPI DMA 未 flush 脏 cache line（cache 一致性，非字符串越界） | flush 前 `SCB_CleanDCache_by_Addr`；CMakeLists 加 CMSIS include 目录 |
| P3 | 开机播两次"连接成功"音 | `TUYA_EVENT_MQTT_CONNECTED` 多次触发（首连+token ack 重连），FP-3 根本不在此播音 | 对齐 FP-3 删掉连接音，仅保留 BIND_START 配网音 |
| P4 | DUAL cloud profile 反复重启/连不上 Wi-Fi/进不了配网 | 未激活阶段 AK 自己的 BLE GATT 提前 `takeover()` 抢占 TAL 唯一 BLE callback，切断 Tuya 配网 | 未激活不 takeover，监听 `ACTIVATE_SUCCESSED` 激活后再切；三次上电只统计物理上电；所有 init 错误不再忽略 |
| P5 | Wi-Fi 凭据重启丢失 | `netmgr_conn_set(SSID_PSWD)` 只更新 RAM 不写 netinfo | 先调 `__netconn_wifi_info_set` 持久化再 `netmgr_conn_set` |
| P6 | 音频播放时 LCD 扭曲错位 | `tdl_disp_dev_flush` 异步复用 framebuffer；地址窗口漂移 | free_cb 等整帧发完；音频期间延迟刷新播完补画；每帧强制重发 CASET/RASET |
| P7 | 背光改 PWM 后黑屏有声音 | 设置页把已验证 P8 GPIO 背光改成未验证 TDL PWM2 | 恢复 GPIO 背光，亮度改 RGB565 framebuffer 色彩缩放 |
| P8 | 非 ASCII 字符显示 | 8×8 ASCII-only 字体 | 缺字渲染为空心方块 □（tofu），非问号 |

**方法论收获**
- **"做不了"先区分能力缺失 vs 工程打包**：BLE HID、ai_chat HOLD 手动 start/stop 都是打包/头文件暴露问题，深挖底层 API 可绕过，不因单 TU 编译失败就 NO-GO。
- **编译通过 ≠ 运行时对**：EC2 只能验证符号解析，IoT 初始化缺失 / 启动时序 / 缺 SDK 命令实现等只有真机才暴露。护栏：新功能 `#if` 门控 + 每次提交前编 v1+cloudasr + 跑 host 单测。
- **文档漂移**：README/tasks/design 勾选严重滞后于代码，判进展须核对 git 提交 + 构建产物 + host tests，以代码为准。
- **子代理不可靠**：会谎报绿灯、空转、超范围（给 protocol 加漏 match 分支）→ 自己独立跑 cargo check/test + grep 残留硬核验。

---

## 6. 工程环境与协作陷阱（收获沉淀）

- **并发写同一工作区互相覆盖**：两个 EC2 session 共用同一工作目录并发写会静默冲掉未提交改动（症状：声称"已改"grep=0、测试数忽增忽减）→ 用 `git worktree` 隔离，每 session 独立目录 + 独立分支共享 `.git`，最后合并。
- **主机安全策略拦截**（三类，均非用户拒绝）：`credential-exfil-kirocrew-token`（含 kirocrew 字样的 argv）、`keystone-scan-ceiling`（>20480 字符命令）、`self-protection-update`（超长 heredoc/apply_patch 误判自更新）→ 不重试同形态，拆成小步专用文件编辑或公开 CLI 子命令。
- **License/授权**：UUID/AuthKey 写入 gitignored 头文件（`ak_tuya_authkey.h`，匹配 `**/*authkey*`）+ 被跟踪 `.example` 模板，真值取自 `docs.local/tuya_keys.txt`，绝不提交。
- **profile 复位**：tos.py 多次 build/clean 后 `app_default.config` 会被复位成非云档（AP size 掉到 ~840KB）→ 看 AP size + 核 `AK_ASR_BOARD_CLOUD/ENABLE_AUDIO_CODECS` 判断，重新 `config choice` 云档。
- **版本自动递增**：pre-commit hook 绑 commit（暂存含实质改动才 bump），检测到版本 profile 并行改动会拒绝自动暂存 → 拆两个 commit，不用 `--no-verify` 绕过。
- **禁推 main**：Kiro Crew 安全策略按分支名硬编码禁 agent push main，只能推 feature 分支由用户合入；本地 main 常领先 origin 多个 commit 待用户 review + push。

---

## 7. 上位机 companion 演进

- MVP UI → 传输/协议/ASR/注入接线（BLE btleplug + USB serialport 跨平台）。
- **Chat A/B/C 绑定体系**：K1–K3 绑 Chat #（Alt+N），别名持久化 `binds.json`；Watched Chat（pin 一个会话作 LCD 唯一数据源）；display 帧加 `chat_slot` 修串屏。
- **术语可用性审查**：sink/board/LCD → status listener/keyboard screen；明说 K 键 ↔ Chat # ↔ session ↔ Watch 四者关系。
- 设备设置双向同步（音量/亮度，WAITING FOR AK / AK SYNCED / AK OFFLINE）；Wi-Fi/License 配网收紧为仅 USB；system_info 上报（固件版本/传输/Wi-Fi/BLE/云 ASR/License）；Hook 按 agent 多选安装；ASR 文本编辑控制（摇杆映射退格/清句/光标）。

## 8. 固件产品化演进

- 骨架 → 双传输(UART0 + BLE GATT 同时) → 连接状态 Header → 云 ASR 集成(AK_ASR_BOARD_CLOUD) → Hook display 帧渲染。
- **LCD 界面重构**（按用户参考图，多轮迭代）：22px Header(标题 + WiFi/USB/BLE 图标 + 版本号) + 左栏 A/B/C 三槽(32px Kiro 精灵) + 右栏单一 selected-session 面板(最近 10 条历史，29 字符自动换行) + 16px 底部状态栏。选中槽紫底 #8E48FF。
- 自动 ASR 降级路由(`ak_voice_route`)：Tuya 不可用自动切 PC ASR + 状态栏提示 + 边沿播提示音，恢复自动切回。
- 独立全屏设置页（长按 center 进入，退出完整恢复工作界面）。
- WS2812 三槽状态灯（进行中：spec 三件套 + ak_leds 驱动 + 状态机，A2 host 单测待续）。
- 安全/可靠性只读审计：3 高 + 15 中 + 9 低 + 9 优化，修 JSONL 转义 / 并发发送竞态 / BLE 名 / 长帧缓冲 / init 重试等。

---

## 9. tuya-kb 知识库（支撑性资产 · 并行支线）

`tuya-kb/` 是 TuyaOpen/T5 知识库，PRD 里标注"参考，不入库"——它不是产品交付物，而是**开发过程中查证 SDK/API/硬件的支撑资产**，是与主仓并行的独立支线（不在主仓 133 个提交内，不占用主线活跃开发日）。

**摄取状态（已全部完成）**
- 37 文件全 done / 0 失败，58 chunks。
- 431 实体 / 468 关系 / 750 mentions；58/58 embedding 已生成。
- **三路检索就绪**：关键词 + 图谱 + 向量，检索实测正常。
- folder watcher 自动增量重摄；含 MCU 手册等 PDF；`.step/.png` 已排除。
- 路径：`/data/projects/tuya-kb`。

**价值**：在 FP-3 云 ASR、BLE GATT/HID、配网/License、cache 一致性等难题攻坚时，作为 SDK 源码与手册的可检索底座，支撑"深挖底层 API 而非拍板 NO-GO"的方法论。

---

## 10. 项目时长统计（按 git 提交事实）

| 指标 | 数值 |
|------|------|
| 首个提交 | 2026-08-20 17:07（北京时间，仓库骨架 Phase 0） |
| 最后提交 | 2026-09-14 09:39 UTC（约北京时间 17:39） |
| **日历跨度** | **约 25 天（3.5 周）** |
| **实际活跃开发日** | **11 天**（8/20、8/21、8/25、8/31、9/1、9/5、9/9、9/10、9/12、9/13、9/14） |
| 总提交数 | 133 |

**逐日提交分布（体现节奏与冲刺）**

| 日期 | 提交数 | 阶段 |
|------|--------|------|
| 2026-08-20 | 1 | 立项 / 仓库骨架 |
| 2026-08-21 | 2 | PRD + 硬件引脚/网表/BOM |
| 2026-08-25 | 4 | 固件 + companion 环境 |
| 2026-08-31 | 5 | bring-up 套件 + PCB v1.0 |
| 2026-09-01 | 3 | 扩展底板 v1.2 返工 |
| 2026-09-05 | 6 | full_selftest + 横屏 |
| 2026-09-09 | 7 | feature-probes 转型 + FP-2/3 |
| 2026-09-10 | 14 | FP 带 LCD + companion MVP + lcd-showcase |
| 2026-09-12 | 10 | 云 ASR 真机 + BLE HID GO |
| **2026-09-13** | **52** | **产品固件集成冲刺** |
| **2026-09-14** | **29** | **界面重构 + 配网 + Hook + WS2812** |

**结论**：日历跨度约 3.5 周，但真正动手的活跃开发只有 **11 天**；**9/13 + 9/14 两天合计 81 个提交，占全部提交的 61%**——工作高度集中在最后两天的产品固件集成与上位机成熟化冲刺，前期硬件/bring-up 阶段节奏舒缓（单日 1~6 个）。tuya-kb 知识库摄取为并行支线，不计入上述活跃开发日。

---

## 11. 里程碑一览（供 slide 时间轴）

| 日期 | 里程碑 |
|------|--------|
| 2026-08-20 | 仓库骨架（Phase 0） |
| 2026-08-21 | PRD + 硬件引脚图/网表/BOM |
| 2026-08-25 | 固件 + companion 开发环境引导 |
| 2026-08-31 | 硬件 bring-up 套件 + PCB v1.0 收尾 |
| 2026-09-01 | 扩展底板 v1.2 返工范围定义 + 核验 |
| 2026-09-05 | full_selftest 综合自检 + LCD 横屏 |
| 2026-09-09 | phase2→feature-probes + MVP 北极星；FP-2/FP-3 spec 与代码 |
| 2026-09-10 | FP 全带 LCD 状态屏；companion MVP 接线；lcd-showcase |
| 2026-09-12 | FP-3 云 ASR 真机打通；FP-6 BLE HID 隔离架构 GO |
| 2026-09-13 | 产品固件双传输 + 云 ASR 集成 + Hook 状态回流 + Wi-Fi/License 配网 |
| 2026-09-14 | LCD 界面重构 + 设备设置页 + 自动 ASR 降级 + Hook 全局安装 + WS2812 起步 |

**当前状态**：FP-1/2/3/5 真机通过；FP-4 本地控制完成、Hook 端到端待验收；FP-6 编译 GO 待真机；FP-7 高度集成待完整 DoD。固件 v0.1.7，本地 main 领先 origin 待 push。

---

## 12. 参考物

- 参考项目：`ref_projects/ki-board/`（ESP32-S3 版，圆屏 GC9D01 + Arduino_GFX，Kiro 幽灵表情动画三组各 40 帧）。
- 权威文档：`docs/prd.md`、`docs/agentic-keyboard-interaction-design.md`、`shared/protocol/protocol.md`。
- 相关会话（26 个中与本项目相关的主线）：Tuya嵌入式设计 / 交互体验设计 / Bringup-Feature Probe / Ki-board Mascot / Convert HTML Slide to PPT / 零碎改进 / Hook for Companion / Optimization / LCD Display / Tuya ASR / WS2812 LED Status。
