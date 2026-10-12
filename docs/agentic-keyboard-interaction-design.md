# Agentic Keyboard 按键与交互设计

> 状态：交互方案草案
>
> 日期：2026-09-09
>
> 适用目标：OpenAI Codex、Kiro CLI、Kiro Crew
>
> 本文重新定义交互，不沿用早期按键功能定义。

---

## 0. v0.3 交互定义（权威，2026-10-12 更新）

> **本章是 v0.3 硬件（6 键 K1–K6 + EC11 旋钮，无五向摇杆）的权威交互定义。**
> 下方第 1–13 章是 2026-09-09 的早期草案（五向摇杆 / K4=PTT / K5=Submit 时代），
> **已过时，仅作设计思路参考**；凡与本章冲突，一律以本章为准。

### 0.1 按键功能（动态管理，按状态切换）

按键在不同状态下承担不同功能，由状态驱动动态管理。基础（空闲）态定义：

| 键 | 单击 | 双击 | 长按 |
|---|---|---|---|
| **K1–K3** | 聚焦当前实例下关注的三个对话（≈ `Alt+1/2/3`） | 切换实例（本地 / 远端，≈ `Ctrl+1/2/3`） | （保留） |
| **K5** | 语音输入（Push-to-Talk / 触发 ASR） | — | — |
| **K4** | 取消：**待确认态**下删除单个字符 | — | 取消：**待确认态**下删除全部（取消本次输入） |
| **K6** | 确认：**待确认态**下确认本次输入 | — | — |
| **EC11 旋钮** | 确认 / 选择（随页面） | 退出测试页 | 进入设置页 |
| **EC11 旋转** | 选择 / 调值（随页面两级模型） | — | — |

> v0.2→v0.3 变更：语音键从 K4 迁到 **K5**；K4 改为**取消**，K6 改为**确认**。
> 已验证的 PTT 云/系统 ASR 链路随语音键一并迁到 K5。

### 0.2 待确认状态（post-ASR，固件主导）

语音输入（K5）结束后进入**待确认状态**。本项目云/Tuya ASR 跑在**固件侧**，所以识别文本由固件获得并发往 PC 端，等待用户确认 / 修改 / 取消——因此**待确认状态由固件主导**：固件持有该状态机、驱动 LED、并把编辑结果（删字符/删全部/确认）同步给 Companion 注入到 PC。

- **K4 单击** = 删除单个字符；**K4 长按** = 删除全部（取消本次输入）。
- **K6 单击** = 确认本次输入（提交 / 下发给 Companion 注入到 PC）。
- 期间 LED 给出手势提示：K4 侧**红色呼吸**（表示长按取消可用），K6 侧**绿色单次连续**（表示单击确认）。
- 退出待确认状态后，相关 LED 熄灭。

> 职责：固件拥有待确认状态机 + ASR 文本 + LED；Companion 负责把确认后的文本注入 PC 输入框，
> 以及（PC ASR 降级场景下）在系统 ASR 不可用时的协同。当语音路由降级为 PC 系统 ASR 时，
> 文本由 Companion 获得，此时待确认态的编辑协同反向进行——但默认 / 云路由下以固件为主导。

### 0.3 LED 语义（颜色 × 效果，二维编码）

LED 用**颜色**表达系统状态、用**闪烁效果**表达手势提示，两者正交组合。

**颜色（状态）：**

| 颜色 | 含义 |
|---|---|
| 绿色 | 空闲 |
| 红色 | 忙碌 |
| 橙色 | 提示，等待操作 |
| 蓝色 | 有信息反馈，无需操作 |

**效果（手势）：**

| 效果 | 含义 |
|---|---|
| 单次连续闪烁 | 单击 |
| 双闪烁 | 双击 |
| 呼吸 | 长按 |

LED 效果引擎由固件实现（主循环 tick 驱动动画）。固件在自己主导的状态（待确认态、本地手势反馈）下**直接驱动** LED；而反映 Agent/云活动的高层状态（忙/空闲/等待/信息）由 Companion 通过协议下发一个**语义级状态**（颜色+效果），固件据此播放，而非 Companion 逐帧控制。7 颗 LED 的分区（整体统一 vs 按键分区，如 K4 侧 / K6 侧）为**待定项**，由后续产品决策确定后实现。

### 0.4 按键功能可配置化（方案 A：Companion 为权威）

**按键功能不写死在固件逻辑中**，而是通过一张 **keymap 配置**管理，权威源在 Companion：

- **配置权威** = Companion 的 keymap 配置文件（扩展现有 `bind_store`，用户可在 UI 编辑）。
- **固件**：把本地动作（原 `#define AK_PTT_KEY_ID "k4"` 等硬编码）抽成一张**运行期 keymap 表**，默认值 = 本章 0.1 定义；可由 Companion 经协议 `keymap` 帧下发覆盖，并缓存进 TAL KV。改键位**无需重新编译固件**。
- **PC 动作映射**（K1–K3→Alt/Ctrl+N、K4 取消、K6 确认、K5 语音触发等 → 具体注入动作）由 Companion 持有，它才知道 `Alt+N`/`Ctrl+N`/SendInput。
- 断开 Companion 时固件用上次缓存 / 默认表。

### 0.5 待定项（需产品决策后实现）

- **实例（本地 / 远端）语义**：K1–K3 双击切"实例"对应 Kiro Crew 的 `Ctrl+1/2/3` 具体含义待确认。
- **LED 7 颗分区**：整体统一表达一个状态，还是按键分区（K4 侧 / K6 侧各若干颗）。

### 0.6 已落地（截至 2026-10-12，分支 `feat/v0.3-bringup`）

- EC11 旋钮：单击确认 / 旋转两级 select-edit / 双击退测试页 / 长按进设置（设置页两级模型已实现）。
- Header 电量图标、标题 Ki-board、设置页硬件测试页（独立循环 + 局部重绘）、电池 ADC（CONTINUOUS + 11 点曲线）均已实现。
- 本章 0.1–0.4 的 keymap 可配置化、0.2 待确认态、0.3 LED 效果引擎：**待实现**（本次改动的目标）。

---


## 1. 设计目标

本键盘不是普通宏键盘，而是 Agent 的物理控制与状态界面，核心目标包括：

- 快速切换多个 Agent 或项目会话。
- 不看 PC 也能识别运行、完成、待审批和错误状态。
- 使用实体按键完成批准、拒绝、中断、继续和语音输入。
- 在 PC 端保留完整的聊天、工具调用、Diff 和审批界面。
- 通过 Companion 适配 Codex、Kiro CLI 与 Kiro Crew，不在 MCU 固件中写死应用快捷键。
- 对持久授权、自动批准和其它高风险操作提供明确的二次确认。

## 2. 参考交互原则

Codex Micro 的核心设计不是给每条命令分配一个宏键，而是：

- Agent Keys 与具体项目或任务绑定。
- Agent Keys 使用颜色表示任务状态。
- Command Keys 负责批准、拒绝、继续、语音和自定义操作。
- Dial/Joystick 用于推理深度、导航和多层映射。

本键盘将这一思路压缩为三个 Agent 槽位、一个大语音键、一个主操作键和五向摇杆。

OpenAI 官方 Codex 文档未公开 Codex Micro 的完整键位协议；Codex Micro 具体硬件行为参考 Work Louder 产品资料和公开兼容实现。Kiro CLI 与 Kiro Crew 功能以 Kiro 官方文档为准。

## 3. 控件定义

### 3.1 总体布局

| 控件 | 主要职责 |
|---|---|
| K1 | Agent 槽位 1 |
| K2 | Agent 槽位 2 |
| K3 | Agent 槽位 3 |
| K4（大按键） | Push-to-Talk 语音输入 |
| K5 | Submit / Continue 主操作 |
| 五向摇杆 | 导航、Reject、Approve、查看详情 |
| LED1–LED3 | 对应 K1–K3 的 Agent 状态 |
| LCD | 会话、审批、工具调用、语音和任务进度 |

### 3.2 K1–K3：Agent Keys

K1–K3 分别对应当前页面中的三个 Agent 槽位。

短按：

- 聚焦对应的项目、会话或任务。
- 如果槽位为空，则创建新会话并绑定到该槽位。
- 按下时必须立即响应，不使用双击判断，保证肌肉记忆。

长按约 800ms：

- 将当前 PC 会话绑定到该槽位。
- 再次长按可进入解绑确认。
- LCD 必须显示绑定目标并要求确认，避免误覆盖。

超过三个会话时，使用控制面板切换下一组三个槽位。LED1–LED3 始终表示 LCD 当前显示的三个槽位。

### 3.3 K4：大尺寸语音键

K4 专用于 Push-to-Talk，不承担 Fn 层或普通宏功能。

交互流程：

1. K4 按下后立即开始录音。
2. LCD 显示录音时长和音量。
3. K4 松开后停止录音并转写。
4. 转写结果进入当前会话输入框。
5. 默认不自动发送，用户按 K5 确认。
6. 摇杆左可丢弃本次转写。

极短误触且没有检测到有效语音时直接丢弃。

各平台对应行为：

- Codex：Push-to-Talk。
- Kiro CLI：Voice mode，建议配置 `voice.autoSubmit=false`。
- Kiro Crew：语音转文字后保留预览，再由 K5 发送。

### 3.4 K5：主操作键

K5 根据当前状态执行主要正向操作：

- 输入框有内容：发送。
- Agent 已完成：Continue。
- 语音转写待确认：发送转写。
- 普通选单：确认当前项目。
- 长按：打开 Agent 控制面板。

K5 不直接完成工具审批，审批使用摇杆左右完成。

### 3.5 五向摇杆

摇杆遵循稳定的方向语义：

| 方向 | 语义 |
|---|---|
| 上/下 | 选择、滚动、提示历史、会话导航 |
| 左 | Back / Reject / Cancel / Stop |
| 右 | Approve once / Continue / Retry |
| 中心 | Details / Confirm |
| 长按中心 | 打开模式与 Agent 控制面板 |

上下文映射：

| 当前状态 | 左 | 右 | 上/下 | 中心 |
|---|---|---|---|---|
| 正在输入 | 取消/清空 | 补全 | 提示历史 | 确认 |
| Agent 运行中 | 中断 | Steering | 活动/队列 | 查看进度 |
| 等待审批 | Reject once | Approve once | 选择审批项 | 查看详情 |
| 任务完成 | Rewind/Fork | Continue | 切换结果 | 查看变更 |
| 错误 | 返回 | Retry | 浏览错误 | 查看日志 |

LCD 底部必须实时显示当前方向含义，避免动态行为造成混淆。

## 4. Agent 控制面板

通过 K5 长按或摇杆中心长按打开控制面板。

建议项目：

1. New Session
2. Fork / Rewind
3. Plan / Goal
4. Crew Mode
5. Activity / Subagents
6. Prompt Optimizer
7. Context
8. Model / Effort
9. Task Runner
10. Cron / Monitor
11. Snapshot
12. Settings

这些功能频率较低，不占用独立物理按键。

## 5. LCD 交互

### 5.1 正常状态

```text
KIRO CLI · PLAN · HIGH
A1 API      A2 WEB      A3 TEST
● Working   ! Input     ✓ Done
←Stop   ↑Activity   OK Detail   →Approve
```

### 5.2 审批状态

```text
APPROVAL REQUIRED
shell: npm run deploy
Risk: HIGH
← Reject    OK Details    → Approve once
```

LCD 应显示：

- 所属 Agent 槽位。
- 工具名称。
- Shell 命令、文件路径或 MCP 工具参数摘要。
- 当前可选审批项。
- 风险等级和是否为持久授权。

### 5.3 语音状态

```text
LISTENING  00:08
██████▃▂
Release: review transcription
```

转写后：

```text
TRANSCRIPTION
Fix the failing auth tests...
K5 Send      ← Discard
```

### 5.4 Task Runner 状态

```text
TASK · auth-migration
Step 3 / 8
Running tests...
←Pause    OK Details    →Open
```

## 6. RGB 状态

LED1–LED3 分别对应当前三个 Agent 槽位。

| 状态 | 颜色/效果 |
|---|---|
| 未绑定 | 熄灭 |
| Idle | 白色 |
| Thinking/Running | 蓝色 |
| Tool running | 蓝色流动或轻微呼吸 |
| Complete 未读 | 绿色 |
| Needs input/Approval | 琥珀色呼吸 |
| Error | 粉红或红色双闪 |
| 当前选中 | 当前颜色叠加短白闪 |
| Companion 断线 | 三灯紫色慢闪 |

完成状态经过数分钟后从绿色回落到白色，避免旧完成状态长期占用注意力。

## 7. Companion 总体架构

固件只发送逻辑事件，不直接发送固定应用快捷键。

```text
Agentic Keyboard
  │ UART / BLE / USB
  ▼
agent-keyboardd
  ├─ Device State Machine
  ├─ Voice / STT
  ├─ Codex Adapter
  ├─ Kiro CLI ACP Adapter
  └─ Kiro Crew Gateway Adapter
```

建议逻辑事件：

```text
FOCUS_SLOT_1
FOCUS_SLOT_2
FOCUS_SLOT_3
BIND_SLOT
APPROVE
REJECT
INTERRUPT
SUBMIT
VOICE_START
VOICE_STOP
OPEN_ACTIVITY
OPEN_DETAILS
OPTIMIZE_PROMPT
EFFORT_UP
EFFORT_DOWN
```

主机端配置使用 JSON，支持：

- 自动识别当前应用或连接目标。
- 每个平台独立映射。
- 用户重映射。
- 多会话槽位与分页。
- USB/BLE 断线恢复。
- Companion 不可用时退化为普通 HID。
- 未识别事件记录和协议版本检查。

## 8. Kiro CLI 集成

### 8.1 推荐方案：ACP

Companion 作为 ACP Client 启动：

```bash
kiro-cli acp
```

Kiro CLI 通过 stdin/stdout 使用 JSON-RPC 2.0。

架构：

```text
Keyboard
  │
agent-keyboardd / ACP Client
  │ JSON-RPC stdio
kiro-cli acp
```

Companion 负责：

- `initialize`
- `session/new`
- `session/load`
- `session/prompt`
- `session/cancel`
- `session/set_mode`
- `session/set_model`

Kiro 会发送：

- `AgentMessageChunk`
- `ToolCall`
- `ToolCallUpdate`
- `TurnEnd`
- `session/request_permission`

Kiro 文档将部分会话更新称为 `session/notification`，ACP v1 标准使用 `session/update`。实现时应根据协议版本兼容两种事件名称。

### 8.2 ACP 审批

示例请求：

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "session/request_permission",
  "params": {
    "sessionId": "sess_123",
    "toolCall": {
      "toolCallId": "call_001"
    },
    "options": [
      {
        "optionId": "allow-once",
        "name": "Allow once",
        "kind": "allow_once"
      },
      {
        "optionId": "reject-once",
        "name": "Reject",
        "kind": "reject_once"
      }
    ]
  }
}
```

批准响应：

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "outcome": {
      "outcome": "selected",
      "optionId": "allow-once"
    }
  }
}
```

实现要求：

- 必须回复原始 JSON-RPC `id`。
- 不假定 `optionId` 固定，应按 `kind` 查找。
- 根据 `toolCallId` 从之前的 ToolCall 更新中查找工具标题和参数。
- 每个会话维护独立审批 FIFO。
- 取消会话时，所有未完成审批必须返回 `cancelled`。

### 8.3 状态映射

```text
session/prompt
    ↓
THINKING
    ↓ ToolCall
TOOL_PENDING
    ↓ request_permission
NEEDS_APPROVAL
    ↓ allow
TOOL_RUNNING
    ↓ completed
THINKING
    ↓ TurnEnd
COMPLETE
```

### 8.4 其它接入方式

| 方案 | 适用性 |
|---|---|
| ACP | 可完整取得状态和审批，正式方案 |
| Hooks | 可观察 Prompt、Tool 前后和 Agent Stop，不能完整接管审批 |
| Headless `stream-json` | 可取得结构化事件，但无交互审批，需要预先授权 |
| 解析 TUI/PTY | 易受布局、颜色和版本变化影响，不推荐 |
| 读取 session JSONL | 适合历史记录，不适合实时审批 |

使用 ACP 时，`kiro-cli acp` 本身不会显示普通 TUI。PC 端界面应由 ACP Client、兼容编辑器或 Companion 的本地 Web/Desktop UI 提供。

## 9. Kiro Crew 集成

Kiro Crew 已有常驻 Gateway、Dashboard、REST API 和 WebSocket，键盘 Companion 可以作为第二个交互界面。

```text
Kiro Crew Dashboard
        │
Kiro Crew Gateway :5476
        │ REST + WebSocket
agent-keyboardd
        │ UART/BLE
Agentic Keyboard
```

PC Dashboard 和实体键盘共享同一个 Gateway：

- PC 批准后，键盘审批提示消失。
- 键盘批准后，Dashboard 同步更新。
- 两端看到同一会话、工具和任务状态。

### 9.1 客户端

Python 原型：

```bash
pip install kirocrew-client
```

```python
from kirocrew_client import CrewClient

async with CrewClient(app_name="agentic-keyboard") as crew:
    slots = await crew.list_slots()
```

默认 Gateway：

```text
http://localhost:5476
```

正式版本建议使用 `app_name` 的 App Secret 换取短期 Token，不长期依赖 localhost 免认证。

### 9.2 WebSocket 事件

| Crew 事件 | 键盘处理 |
|---|---|
| `slots` | 更新 K1–K3 会话绑定 |
| `slot_title` | 更新 LCD 会话标题 |
| `chat_message` | 收到新消息 |
| `chat_chunk` | Agent 输出中，蓝灯 |
| `tool_call` | 更新当前工具 |
| `approval` | 琥珀灯并显示审批 |
| `chat_done` | 绿灯 |
| `chat_error` | 红灯 |
| `subagent_done` | 子代理完成 |
| `task_update` | Task Runner 进度 |
| `task_complete` | 长任务完成 |
| `notification` | 普通通知 |
| `proactive_notification` | Cron/Heartbeat 通知 |

开发阶段使用 `onRaw()` 记录原始事件，确认当前 Crew 版本的字段格式。

### 9.3 Crew 审批

优先使用唯一审批 ID：

```python
await crew.resolve_approval(approval_id, True)
await crew.resolve_approval(approval_id, False)
```

兼容旧式接口：

```python
await crew.approve_action(slot_id, task_id)
await crew.reject_action(slot_id, task_id)
```

### 9.4 会话与子代理

```python
await crew.list_slots()
await crew.create_slot("API")
await crew.get_slot_history(slot_id)
await crew.send_message(slot_id, message)

await crew.spawn("review the API changes")
await crew.list_subagents()
await crew.get_subagent_status(subagent_id)
```

每个 Agent 槽位保存：

```json
{
  "key": "K1",
  "slotId": "slot-api",
  "title": "API refactor",
  "state": "approval",
  "approvalId": "approval-123"
}
```

### 9.5 断线恢复

WebSocket 断线时：

1. LCD 显示 `CREW OFFLINE`。
2. 三颗 LED 紫色慢闪。
3. Companion 自动重连。
4. 重连后调用 `list_slots()`。
5. 重建槽位状态。
6. 拉取待处理审批、子代理和任务状态。

不能只依赖增量事件，否则 Gateway 重启后键盘状态会丢失。

## 10. PC 界面

### 10.1 Kiro CLI ACP

`kiro-cli acp` 不会自动创建可视窗口。Companion 应提供：

- 本地 Web UI，例如 `http://127.0.0.1:8765`。
- 原生桌面 UI。
- 或连接 JetBrains/Zed 等 ACP Client。

自建 UI 可显示：

- 多会话标签。
- 流式回复。
- ToolCall 和审批。
- Diff、错误、上下文和模型。
- Stop、Retry、Fork。

### 10.2 Kiro Crew

继续使用 Crew Dashboard 即可。键盘只是同一 Gateway 上的额外交互终端，不需要复制完整 Dashboard。

## 11. 安全约束

- 摇杆右仅默认执行 `approve_once`。
- `allow_always`、`reject_always` 必须进入详情并长按确认。
- 不为实体键配置一键 YOLO/Trust All。
- 高风险命令必须在 LCD 显示命令和目标路径。
- Companion 必须校验审批 ID、会话 ID 和工具调用 ID。
- 多个审批同时到达时按会话维护队列。
- PC 与键盘任一端处理审批后，另一端立即失效该请求。
- Gateway Token、App Secret 和 API Key 不存储在 MCU。
- Companion 断线或协议版本不兼容时，禁止执行审批操作。

## 12. 实施阶段

### 阶段 1：设备输入与显示

- K1–K3 会话槽位。
- K4 Push-to-Talk。
- K5 Submit。
- 摇杆导航、批准和拒绝。
- LCD 页面与 LED 状态机。

### 阶段 2：Kiro Crew 原型

- 使用 `kirocrew-client`。
- 订阅 slots、chat、tool、approval 和 task 事件。
- 实现键盘审批和断线恢复。
- 继续使用现有 Crew Dashboard。

### 阶段 3：Kiro CLI ACP

- 实现 JSON-RPC Client。
- 完成 session、ToolCall、TurnEnd 和审批。
- 提供本地 Web UI。

### 阶段 4：Codex Adapter

- 对接 Codex 可用的任务与状态接口。
- 无稳定接口时仅提供可配置快捷键与前台窗口切换。
- 不依赖未公开的 Codex Micro 私有协议作为必要功能。

### 阶段 5：正式 Crew App

- 增加 Dashboard 配置页。
- 提供槽位绑定、键位映射、LED颜色和设备诊断。
- 使用 App Secret 自动鉴权。

## 13. 参考资料

- [OpenAI Docs：Codex App](https://developers.openai.com/codex/app/)
- [Work Louder：Codex Micro](https://worklouder.cc/codex-micro)
- [Kiro CLI Terminal UI](https://kiro.dev/docs/cli/terminal-ui/)
- [Kiro CLI ACP](https://kiro.dev/docs/cli/acp/)
- [ACP Tool Calls and Permission Requests](https://agentclientprotocol.com/protocol/v1/tool-calls)
- [Kiro Permissions](https://kiro.dev/docs/permissions/)
- [Kiro Hooks](https://kiro.dev/docs/hooks/types/)
- [Kiro Crew SDK/API](https://kiro.dev/docs/crew/apps/sdk/)
- [Kiro Crew Sessions](https://kiro.dev/docs/crew/chat/sessions/)
- [Kiro Crew Message Controls](https://kiro.dev/docs/crew/chat/message-controls/)
- [Kiro Crew Security](https://kiro.dev/docs/crew/security/)
- [Kiro Crew Subagents](https://kiro.dev/docs/crew/features/subagents/)
- [Kiro Crew Task Runner](https://kiro.dev/docs/crew/features/task-runner/)
