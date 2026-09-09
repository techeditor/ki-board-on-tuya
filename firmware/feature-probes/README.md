# feature-probes（功能验证探针 · Feature Probes）

Bring-up（`firmware/bringup/`）验证的是**硬件基础功能**——每个外设单点通不通
（LCD / WS2812 / 按键 / 串口 / 无线自检）。

**feature-probes** 是 bring-up 之后的一层：**每个 probe 是一个独立的、只做验证的
单功能实现**，贴近最终 agentic keyboard 的真实功能。它们不是零散验证，而是**逐步
拼出一个 MVP demo**。

## MVP demo 目标（北极星）

一个**无线 AI 语音小键盘，用来控制 Kiro Crew**。四条基本要求：

1. **无线连接** —— 板子不走 USB 线，走 **BLE GATT** 连到 PC 上的 companion。
2. **以控制 Kiro Crew 为目标** —— 识别到的文字/命令经 companion 路由进 Kiro Crew。
3. **板载 Mic 走涂鸦云 ASR** —— 板载麦克风 → 涂鸦云 ASR（`ai_chat` ONESHOT →
   `AI_USER_EVT_ASR_OK` 取文字）。
4. **也支持激活电脑端自身的 ASR** —— 一个按键让 companion 激活 Windows 系统 ASR，
   识别文字同样进 Kiro Crew。

数据流：

```
板子(Mic + 按键 + LCD) --BLE GATT--> PC companion --> Kiro Crew
  · 语音键  → 板载Mic → 涂鸦云ASR → 文字 → BLE GATT → companion → Kiro Crew
  · ASR键   → BLE GATT 信号 → companion 激活 Windows 系统ASR → 文字 → Kiro Crew
  · 状态/通知 ← companion ← Kiro Crew 事件（复用 FP-1 显示层）
```

> MVP 主路走 **BLE GATT + companion**（富交互，支持"控制 Kiro Crew"的四类操作）。
> **BLE HID 直连**作为**最基本兜底方案**也在 feature-probes 范围内（FP-6）：无 companion
> 时板子直接当标准键盘，向焦点窗口输入文字 + 回车发送 + 快捷键切换，能力受限但零依赖。

## 探针路线图（Roadmap · 朝 MVP 递进）

| ID | 名称 | 验证的功能链路 | 状态 | Spec |
|---|---|---|---|---|
| **FP-1** | **agent-display** | LVGL 场景 + 吉祥物动画 + 通知 UI，按键模拟切换 Agent 状态/通知（移植自 CodeTank）。MVP 的显示层 | ✅ 完成（含真机观感） | `.kiro/specs/feature-probes-agent-display/` |
| **FP-2** | **ble-gatt-link** | BLE GATT 板↔companion：自定义 GATT 服务，notify 上报按键/文字帧、write 下发状态。**无线地基，FP-3/4/5 都挂在它上** | 🔨 已实现，待真机验收 | `.kiro/specs/feature-probes-ble-gatt-link/` |
| **FP-3** | **tuya-cloud-asr** | 板载 Mic → 涂鸦云 ASR（`ai_chat` HOLD/PTT）→ `AI_USER_EVT_ASR_OK` 取文字 → UART0 JSONL | 🔨 已实现（D8 派生自 your_chat_bot，BUILD SUCCESS，ASR→UART），待真机验收 | `.kiro/specs/feature-probes-tuya-cloud-asr/` |
| **FP-4** | **kirocrew-control** | companion 驱动 Kiro Crew 的四类操作：①绑定 agent/对话 ②输入文字 ③发送/执行 ④切换 agent/对话 | 📋 spec 完成（路线 A：本机 session-control API） | `.kiro/specs/feature-probes-kirocrew-control/` |
| **FP-5** | **pc-asr-activate** | 板键 → companion 激活 Windows 系统 ASR → 文字 → Kiro Crew | ⏳ 规划 | — |
| **FP-6** | **ble-hid-fallback** | **兜底方案**：无 companion 时板子直接当标准 BLE HID 键盘，向焦点窗口输入文字 + 回车发送 + 快捷键切换对话（NimBLE 手搓 HID 描述符 + SM/配对） | 📋 spec 完成 · 可行性 **NO-GO**（技术可行但高风险，待专门排期 spike） | `.kiro/specs/feature-probes-ble-hid-fallback/` |
| **FP-7** | **mvp-integration** | 无线 + 双 ASR + 控制 Kiro Crew 端到端串起来 | ⏳ 规划 | — |

> 顺序：FP-2 是地基，先做；FP-3（板端云 ASR）与 FP-4/FP-5（companion 侧）之后可并行；
> FP-6 BLE HID 兜底相对独立（走原生 NimBLE，与 FP-2 的 TAL GATT 是两条 BLE 路线，分属不同
> probe 固件）；最后 FP-7 整合。FP-1 的显示层在 FP-7 复用为状态/通知区。
>
> **两条控制 Kiro Crew 的路线**：主路 = FP-2 GATT + companion（富交互，支持 FP-4 四类操作）；
> 兜底 = FP-6 BLE HID 直连（无需 companion，靠键盘输入+快捷键，能力受限）。

## 目录约定（与 bring-up 一致）

```
firmware/feature-probes/<probe_name>/
├── src/tuya_app_main.c      # 入口
├── config/TUYA_T5AI_CORE.config
├── app_default.config
├── CMakeLists.txt
└── README.md                # 该 probe 的目的/操作/预期
```
共享代码放 `firmware/feature-probes/common/`（如需），spec 放 `.kiro/specs/feature-probes-*/`。

## 构建

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/<probe_name>
tos.py config choice   # TUYA_T5AI_CORE.config
tos.py build           # 改 .config 后先 tos.py clean -f
```
纯逻辑模块带 host 单测，沿用 bring-up/app 的 `run_host_tests.sh` 模式。
