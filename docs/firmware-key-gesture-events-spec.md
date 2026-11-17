# 固件规格：按键手势事件 + 槽位状态颜色

> 状态：v0.3 已实施（手势部分 2026-11-16 起按本文；v0.2 的 K1–K3 `click`/`dclick` 版本见 tag `v0.2-final`）。
>
> 本文件包含两个相互独立的固件功能：
> 1. **按键手势事件（single / double / long）**：第 1–6 节。
> 2. **槽位状态颜色（左侧三块图标背景色）**：文末「附」一节。
>
> 线上帧格式以 `shared/protocol/protocol.md` 的 `key` 一节为准，本文写清语义来源与实现位置。
>
> 关联文件：
> - 手势分类器：`firmware/app/common/include/ak_input_fsm.h`、`firmware/app/common/src/ak_input_fsm.c`
> - keymap：`firmware/app/common/include/ak_keymap.h`、`firmware/app/common/src/ak_keymap.c`
> - 分发与上报：`firmware/app/src/tuya_app_main.c`（`on_input_event`、`dispatch_gesture_for_slot`、主循环 gesture tick）
> - 帧编码：`ak_proto_key_gesture()`（`firmware/app/common/src/ak_protocol.c`）
> - 固件单测：`firmware/app/test/test_ak.c`（`test_gesture`、`test_keymap`、`test_protocol`）
> - Companion：`companion/src-tauri/src/protocol.rs`（`KeyPayload.bind`）、`kirocrew_control.rs`（`route_input`）、`lib.rs`（`route_board_via_crew_api`）

---

## 1. 适用范围

- 手势适用于 K1–K6 中 keymap `kind=normal` 的键，以及 EC11 按压（`enc`）。
- 语音键（`kind=voice`，默认 K5）不走手势：它要按下立即开始录音，所以上报原始 `down` / `up`。
- EC11 旋转是 `encoder` 帧，不属于手势。

## 2. 三种手势（互斥，每次物理操作只产生一个）

| 手势 | 成立条件 | 上报时刻 |
|---|---|---|
| `single` | 按下后在长按阈值前松开，且松开后双击窗口内没有第二次按下 | 双击窗口到期时 |
| `double` | 第一次松开后、双击窗口内开始第二次按下，并在长按阈值前松开 | 第二次松开时；这一对不再报 `single` |
| `long` | 按住达到长按阈值 | 达到阈值时立即报一次；之后的松开不再报任何手势 |

固件参数（改动时同步本表）：

| 参数 | 值 | 宏 |
|---|---|---|
| 去抖 | 20 ms | `AK_FSM_DEBOUNCE_MS` |
| 长按阈值 | 800 ms | `AK_FSM_LONG_PRESS_MS` |
| 双击窗口（第一次松开 → 第二次按下） | 250 ms | `AK_GESTURE_DOUBLE_WINDOW_MS` |

推论：

- `single` 比松开晚最多 250 ms，这是区分单击和双击的代价。需要零延迟的键应设成 `voice` 或只绑 `long`。
- 第二次按下在窗口内开始、但按得比窗口久，仍算 `double` 的候选；按住到 800 ms 则见下一条。

## 3. 时序规则

1. 短按，然后在窗口内又按住不放：两次操作，**先报 `single`，再报 `long`**。
   实现：`LONG` 边沿到来时 `ak_gesture_feed()` 先返回欠下的 `SINGLE`，`LONG` 记为 owed，下一次 `ak_gesture_tick()`（下一个主循环周期）返回。
   这修正了 v0.3 早期实现里「PC 先收到 long 再收到单击」的问题（固件审查 FW-26）。
2. 第二次按下晚于窗口、但 tick 还没来得及让 `single` 到期：这次 `DOWN` 立即返回上一次的 `single`，新按下重新开始判定。
3. 设置页打开期间（本地模态），任何到期的手势都丢弃，不上报。
4. `enc` 的 `long` 在主页面打开设置页（本地动作），仍按规则上报一帧，`bind:"settings"`。

## 4. 帧格式与 bind

```json
{"v":1,"seq":7,"type":"key","payload":{"id":"k1","action":"single","bind":"chat1","held_ms":0}}
{"v":1,"seq":8,"type":"key","payload":{"id":"k1","action":"long","held_ms":0}}
```

- `action`：手势名。`bind`：这个 (键, 手势) 在 keymap 里绑定、且这次真正生效的动作名；未绑定或不生效时省略。
- 生效规则：`chatN` / `instanceN` 总是生效；`cancel_char` / `cancel_all` / `confirm` 只在待确认态（post-ASR）生效，
  固件同时退出待确认态（`cancel_all` / `confirm`）。不在待确认态时这些键只上报手势，不带 `bind`。
- 单击 K1–K3 且绑定为 `chatN` 时，固件在 LCD 上本地高亮对应 A/B/C 槽位。

## 5. Companion 路由

先看 `bind`，没有 `bind` 再用默认规则：

| 帧 | 动作 |
|---|---|
| `bind: chatN` | K1–K3 且 N=1..3：已连 Crew API 时选中 Chat A/B/C 稳定槽位（该槽设了 Alt+N 时再跳转）；未连或该槽不是 API 槽时注入 `Alt+N` |
| `bind: instanceN` | `Ctrl+N` |
| `bind: cancel_char` / `cancel_all` / `confirm` | `Backspace` / `Ctrl+A` 后 `Backspace` / `Enter` |
| 无 bind，K1–K3 `single` | 已连 Crew API 时选中该键的 A/B/C 槽位 |
| 无 bind，K1–K3 `long` | 长按自动绑定（UI 里选中的对话 → 该键槽位） |
| 无 bind，`enc` `single` | `Enter` |
| 其他（含 `bind:"settings"`） | 忽略 |

Crew API + WebSocket 是主通道，快捷键注入只在 API/WS 不可用、或目标不是 API 槽位时兜底。

## 6. 测试

- 固件 `test_gesture`：single 只在窗口到期后报一次；double 不再报 single，且第二次按下持续超过窗口仍判 double；
  long 在阈值立即报、松开不报；「短按 + 窗口内按住」先 single 后 long；窗口外的第二次按下先补上一次的 single。
- 固件 `test_protocol`：带 `bind` 与不带 `bind` 的手势帧编码。
- Companion `protocol.rs`：解析带/不带 `bind` 的手势帧，`dclick` 解析为 `Unknown`。
- Companion `kirocrew_control.rs`：bind 优先路由、`enc single → Enter`、未绑定手势在路由层为 no-op。

---

# 附:槽位状态颜色（左侧三块图标背景色）

> 本节是本规格文档的第二个独立功能，与 single/double/long 手势相互独立，可分别实施。
> 固件侧在云端主机执行；Companion 侧状态词汇调整由本地负责。

## A1. 需求

读到 agent 状态后，按下表给**键盘左侧三块槽位图标（Chat A / B / C）的背景色**着色：

| 颜色 | 语义 | RGB565 常量（建议） |
|------|------|---------------------|
| 紫色 | 未绑定 / 默认 | `LCD_SESSION_SELECTED_BG`（已有，0x8E48FF）或新增 `AK_COL_PURPLE` |
| 绿色 | 空闲 | `AK_COL_GREEN` |
| 红色 | 忙碌 | `AK_COL_RED` |
| 橙色 | 提示，等待操作 | `AK_COL_ORANGE` |
| 蓝色 | 有信息反馈，无需操作 | `AK_COL_BLUE`（或 `AK_COL_CYAN`，择一并统一） |

## A2. 状态词汇对齐（Companion → 固件）

`display` 帧的 `state` 字段驱动颜色。为覆盖 5 种语义，统一为以下取值：

| state 字符串 | 颜色 | 产出方 |
|--------------|------|--------|
| （空 / `-` / 未收到帧） | 紫色 | 固件默认；解绑时 Companion 发 `unbound` |
| `unbound` | 紫色 | Companion（解绑某槽时主动发一帧） |
| `idle` | 绿色 | Companion（hook `stop` / crew 空闲） |
| `running` | 红色 | Companion（agent 运行中 / crew running / stopping） |
| `waiting` | 橙色 | Companion（crew `pending_approval` / `needs_input` / `waiting_for_input`） |
| `info` | 蓝色 | Companion（**新增**：有新回复可读、无需操作） |

> 注意：这与现有 `ak_status_color()` 的旧映射不同（旧版 `running`→绿、`idle`→灰、
> `tool`→橙、`received`→青）。本规格**以上表为准**，需同步改固件和 Companion。

### A2.1 Companion 侧改动（本地负责）

文件 `companion/src-tauri/src/status_sink.rs`（`map_hook_event`）与
`companion/src-tauri/src/crew_api.rs`（`slot_display_status`）：

- `map_hook_event`：
  - `agentSpawn` / `sessionStart` → `running`（忙碌）
  - `postToolUse` → `running`（仍在忙，归忙碌）
  - `userPromptSubmit` → `running`
  - `stop`：若有回复文本 → **`info`**（有反馈可读、无需操作，蓝色）；
    若无文本 → `idle`（空闲，绿色）
  - 其它未知 → `running`
- `slot_display_status`（crew API 投影）：
  - `pending_approval || needs_input || waiting_for_input` → `waiting`（橙）
  - `stopping || running || subagents_running` → `running`（红）
  - 有 `last_message` 且非忙 → `info`（蓝，有反馈无需操作）
  - 否则 → `idle`（绿）
- **解绑**：当某 Chat A/B/C 槽位的 session 绑定被清除时（`kc_bind_session_clear`
  等路径），Companion 向该 `chat_slot` 发送一帧 `state:"unbound"` 的 `display`，
  使固件把该槽图标恢复紫色。（若板子离线则忽略。）

> `error` 状态（协议已定义）暂不单独着色，可并入红色或后续扩展；本期不处理。

## A3. 固件改动清单

### A3.1 `ak_status_color()`（`tuya_app_main.c`，约 763 行）

改为本规格的映射：

```c
static uint16_t ak_status_color(const char *state)
{
    if (state == NULL || state[0] == '\0' || strcmp(state, "-") == 0 ||
        strcmp(state, "unbound") == 0) {
        return LCD_SESSION_SELECTED_BG;   /* 紫：未绑定/默认 */
    } else if (strcmp(state, "idle") == 0) {
        return AK_COL_GREEN;              /* 绿：空闲 */
    } else if (strcmp(state, "running") == 0) {
        return AK_COL_RED;                /* 红：忙碌 */
    } else if (strcmp(state, "waiting") == 0) {
        return AK_COL_ORANGE;             /* 橙：等待操作 */
    } else if (strcmp(state, "info") == 0) {
        return AK_COL_BLUE;               /* 蓝：有反馈无需操作 */
    }
    return LCD_SESSION_SELECTED_BG;       /* 兜底按未绑定（紫） */
}
```

> 若希望紫色语义更明确，可在 `ak_gfx.h` 增加 `#define AK_COL_PURPLE AK_RGB565(0x8EU,0x48U,0xFFU)`
> 并替换上面的 `LCD_SESSION_SELECTED_BG`。两者等价，择一即可。
> 注意 `ak_status_color()` 当前还被 `ak_render_status()` 用于正文「Status: …」文字颜色
> （约 1315 行）——改这里会同时影响那行文字颜色。若希望正文文字另着色，
> 可拆成两个函数（`ak_slot_bg_color` 用于图标背景，正文沿用旧逻辑）；
> **推荐统一用新映射**，保持视觉一致。

### A3.2 `ak_draw_session_sidebar()`（约 1233 行）

当前每块图标背景只按「是否选中」：

```c
uint16_t bg = selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
```

改为按**该槽位的 agent 状态**着色（`s_sessions[i].state` 已逐槽保存）：

```c
/* 背景色 = 该槽位 agent 状态色；选中态用更亮边框/前景区分，而不是覆盖背景。 */
uint16_t bg = ak_status_color(s_sessions[i].state);
uint16_t fg = selected ? AK_COL_WHITE : AK_COL_GRAY;
```

- 选中态的区分：不要再用背景色表示选中（背景已被状态色占用）。改用：
  - 选中块画一圈高亮边框（如 `AK_COL_WHITE` 1px outline），或
  - 选中块前景 `fg` 用白色、未选中用灰色（现有 `fg` 逻辑可保留）。
  - 具体视觉由实现者定，但**务必保证状态色可见且选中态仍可辨识**。
- 三槽默认 `s_sessions[i].state` 初值为 `"idle"`（见约 810 行 `{.agent="-", .state="idle"}`）。
  **按本规格，未绑定应为紫色**，因此把默认初值从 `"idle"` 改为 `""`（空）或 `"unbound"`，
  使启动时三块为紫色，收到真实 display 帧后再变色。

### A3.3 重绘时机

槽位状态在收到 `display` 帧更新 `s_sessions[slot].state` 后，需触发 sidebar 重绘
（现有 `ak_render_status()` / `s_lcd_redraw_pending` 机制）。确认收到 `display`
帧的处理路径会重画 sidebar（当前 `ak_render_status()` 已调用 `ak_draw_session_sidebar()`）。

## A4. 验收标准

- [ ] 未绑定（启动后未收到某槽 display 帧，或收到 `unbound`）→ 该槽图标**紫色**。
- [ ] 收到该槽 `state:"idle"` → **绿色**；`running` → **红色**；`waiting` → **橙色**；
      `info` → **蓝色**。
- [ ] 选中态仍可辨识（边框或前景色），不与状态色冲突。
- [ ] Companion：`stop` 有文本→`info`（蓝）、无文本→`idle`（绿）；crew `waiting` 类→橙；
      解绑槽位→发 `unbound` 帧→该槽回紫。
- [ ] 固件 host 单测：为 `ak_status_color()` 新映射补断言
      （`""`/`unbound`→紫、`idle`→绿、`running`→红、`waiting`→橙、`info`→蓝）。
- [ ] `cargo check` + `npm run typecheck`（Companion 状态词汇改动）。

## A5. 落地顺序

1. 固件 `ak_status_color` + sidebar 着色 + 默认紫，云端主机编译烧录，灌入模拟 `display`
   帧验证五色。
2. Companion 调整 `map_hook_event` / `slot_display_status` 产出新词汇 + 解绑发 `unbound`。
3. 两侧联调：真实 Kiro Crew 跑一轮，确认 A/B/C 图标随 agent 状态变色。
