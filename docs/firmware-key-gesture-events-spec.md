# 固件规格：按键手势事件 + 槽位状态颜色

> 状态：待实施（固件侧在 EC2 云端执行；Companion 侧由本地负责）
>
> 本文件包含两个相互独立、可分别实施的固件功能：
> 1. **按键手势事件（click / dclick / long）** —— 正文第 1–9 节。
> 2. **槽位状态颜色（左侧三块图标背景色）** —— 文末「附」一节。
>
> 功能 1 意图：把 K1–K3 的「跳转」语义从物理按下（`down`）改为**单击**（`click`），
> 从根上保证**长按（`long`）不会触发跳转**，并为 K1–K3 补齐 **click / dclick / long**
> 三种语义事件帧。
> 关联文件：
> - 协议源：`shared/protocol/protocol.md`
> - 固件 FSM：`firmware/app/common/src/ak_input_fsm.{c,h}`
> - 固件输入分发：`firmware/app/common/src/ak_input.c`、`firmware/app/src/tuya_app_main.c`
> - 固件协议编码：`firmware/app/common/src/ak_protocol.c`、`.../include/ak_protocol.h`
> - 固件 host 单测：`firmware/app/test/test_ak.c`
> - Companion 协议解码：`companion/src-tauri/src/protocol.rs`
> - Companion 路由：`companion/src-tauri/src/lib.rs`、`kirocrew_control.rs`

---

## 1. 动机

当前链路：物理键 **K1/K2/K3 的 `down`** 在 PC 端（Companion）直接触发
「跳转 / 选中稳定槽位」。而固件对一次长按会依次发出 `down → long(800ms) → up`，
因此**长按 K1–K3 时 `down` 会先触发一次跳转**，再在 800ms 后发 `long`。

Companion 已实现「长按 K1–K3 = 把 UI 选中的对话自动绑定到 Chat A/B/C」。
但由于 `down` 先行，长按会先误跳转一次，手感不佳。

**彻底解决方案（本规格）**：在固件侧区分单击/双击/长按三种语义：

- 短按释放 → `click`（跳转发生在**松手**而非按下）
- 400ms 窗口内第二次短按释放 → `dclick`（双击）
- 按住超过长按阈值 → `long`（长按，用于绑定）；长按**不会产生 click**

PC 端把 K1–K3 的跳转触发点从 `down` 改为 `click`，于是**长按绝不跳转**，
问题从根上消失，且不依赖任何「配置状态」开关。

---

## 2. 关键设计原则（务必遵守）

1. **不破坏现有 `down`/`up`/`long` 事件流。**
   K4（push-to-talk 语音）、nav 摇杆（编辑/翻页）、center（设置页）、ASR 等
   路径都依赖 `up` **立即**到达、以及现有 `down`/`long` 时序。手势识别是**叠加层**，
   绝不替换或延迟 FSM 现有输出。

2. **click 必须延迟判定。**
   单击要能确认「不是双击」，必须等双击窗口（`AK_DCLICK_WINDOW_MS = 400ms`）
   超时后才能确认。因此手势识别器需要靠 `now_ms` **tick 推进**来在窗口到期时
   吐出 `click`（不能纯事件驱动，因为窗口超时本身没有输入事件）。

3. **语义手势只加在 K1 / K2 / K3。**
   K4 / K5 / nav / center 维持原样，不加 click/dclick。
   - K5（提交）继续用 `down` 即时触发，**不要**改成 click（提交不应延迟到松手）。
   - K4 继续用现有 `down`/`long`/`up` 的 push-to-talk 逻辑 + 现有 `ak_dclick`
     本地提示音逻辑，**不受影响**。

4. **新增帧与旧帧并存。**
   K1–K3 仍然照常发送 `down` / `up`（供 LOGS、UI 的 `lastKey` 高亮等使用），
   **额外**发送语义帧 `click` / `dclick` / `long`。
   注意：`long` 事件 FSM 本来就会发（当前已作为 `action:"long"` 的 key 帧上报），
   手势层对 K1–K3 的 `long` 不需要重复发帧——沿用 FSM 现有的 `long` 帧即可。
   （见 §5 的「事件去重」说明。）

5. **纯 C、host 可测。**
   手势识别器与 FSM 一样做成 pure、无 SDK 依赖，并在 `firmware/app/test/test_ak.c`
   补齐单测。固件在 EC2 云端编译 + 本地烧录验证；本机只跑 host 单测。

---

## 3. 协议变更（`shared/protocol/protocol.md`）

在 `### key（机械按键）` 小节，把 `action` 取值扩展为：

- `action`：`down` / `up` / `long` / `click` / `dclick`
  - `down` / `up`：原始电平边沿（所有键，保持不变）。
  - `long`：按住超过长按阈值（所有键，保持不变）。
  - **`click`**：一次短按的释放，且在双击窗口内未发生第二次按下。**仅 K1–K3 产生**。
  - **`dclick`**：双击窗口内的第二次短按释放。**仅 K1–K3 产生**。

补充说明文字（建议原文加入协议文档）：

> `click` / `dclick` 为 K1–K3 的语义手势，叠加在 `down`/`up` 之上：一次短按会先发
> `down`、再发 `up`，随后（若非双击）在约 400ms 后发 `click`；双击则在第二次释放时
> 发 `dclick`。长按发 `long`，不会再发 `click`/`dclick`。K4/K5/nav 不产生
> `click`/`dclick`。接收方必须忽略无法识别的 `action`（向后兼容）。

示例帧：

```json
{"v":1,"seq":30,"type":"key","payload":{"id":"k1","action":"click","held_ms":40}}
{"v":1,"seq":31,"type":"key","payload":{"id":"k2","action":"dclick","held_ms":35}}
```

---

## 4. 固件改动清单

### 4.1 `ak_input_fsm.h` — 事件枚举 + 手势识别器声明

在 `ak_input_event_t` 增加两个成员（**追加在末尾**，保持既有数值稳定）：

```c
typedef enum {
    AK_INPUT_EVENT_NONE = 0,
    AK_INPUT_EVENT_DOWN,
    AK_INPUT_EVENT_UP,
    AK_INPUT_EVENT_LONG,
    AK_INPUT_EVENT_CLICK,   /* 新增：确认的单击（短按，非双击，窗口已过） */
    AK_INPUT_EVENT_DCLICK,  /* 新增：确认的双击 */
} ak_input_event_t;
```

新增「单击/双击手势识别器」类型与 API（与 `ak_dclick` 同风格，pure）：

```c
/* 手势识别器：喂入 FSM 的 DOWN/UP/LONG 事件 + tick，产出 CLICK/DCLICK。
 * - 短按释放后进入「待定 click」，等待 AK_DCLICK_WINDOW_MS：
 *     窗口内再来一次短按释放 -> 立即返回 DCLICK（取消待定 click）；
 *     窗口到期无第二次       -> 返回 CLICK。
 * - 本次按下触发过 LONG -> 取消一切待定，不产生 CLICK/DCLICK。
 * 需要每个 tick 调用 ak_gesture_tick() 以便窗口到期时吐出 CLICK。 */
typedef struct {
    bool     pending;        /* 有一次短按释放在等待双击窗口 */
    uint32_t pending_since;  /* 该释放的时间戳 */
    bool     press_had_long; /* 本次按下已触发 LONG（排除） */
} ak_gesture_t;

void ak_gesture_init(ak_gesture_t *g);

/* 喂入一个 FSM 事件；返回本次立刻可确定的手势事件（通常是 DCLICK），
 * 否则返回 AK_INPUT_EVENT_NONE。held_ms 来自 FSM。 */
ak_input_event_t ak_gesture_feed(ak_gesture_t *g, ak_input_event_t event,
                                 uint32_t now_ms, uint32_t held_ms);

/* 每个轮询 tick 调用；窗口到期时返回 CLICK，否则 NONE。 */
ak_input_event_t ak_gesture_tick(ak_gesture_t *g, uint32_t now_ms);
```

> 说明：复用现有 `AK_DCLICK_WINDOW_MS`（400ms）作为双击窗口。
> `ak_input_event_str()` 需要能把 `CLICK`/`DCLICK` 转成 `"click"`/`"dclick"`。

### 4.2 `ak_input_fsm.c` — 实现

- `ak_input_event_str()` 增加两个分支：
  ```c
  case AK_INPUT_EVENT_CLICK:  return "click";
  case AK_INPUT_EVENT_DCLICK: return "dclick";
  ```
- 实现 `ak_gesture_init / ak_gesture_feed / ak_gesture_tick`：
  - `feed(DOWN)`：`press_had_long = false`。（不清除 pending —— 第二次按下尚未确定是不是双击的释放。）
  - `feed(LONG)`：`press_had_long = true; pending = false;`（长按取消待定 click，不产生手势）。
  - `feed(UP)`：
    - 若 `press_had_long`：复位 `press_had_long=false`，返回 `NONE`（长按释放不算手势）。
    - 否则若 `pending` 且 `now - pending_since <= AK_DCLICK_WINDOW_MS`：
      `pending=false`，返回 `DCLICK`。
    - 否则：`pending=true; pending_since=now;` 返回 `NONE`（待定 click）。
  - `tick(now)`：若 `pending` 且 `now - pending_since > AK_DCLICK_WINDOW_MS`：
    `pending=false`，返回 `CLICK`；否则 `NONE`。

> 逻辑与现有 `ak_dclick_feed` 高度一致，差别是：这里要**产出 click**（需要 tick 到期），
> 而 `ak_dclick` 只产出「是否双击」的布尔。可以保留 `ak_dclick` 不动（K4 仍用它）。

### 4.3 `ak_input.c` — 仅对 K1–K3 挂手势识别器

当前 `ak_input_poll()` 对每个输入跑 FSM 并回调。改动：

- 在 `ak_input_t` 里为 K1–K3 增加一个 `ak_gesture_t gesture`（nav/K4/K5 不需要）。
  可以给**所有** key 都带一个 gesture 字段简化结构，但**只对 K1–K3 使用**。
- `ak_input_init()`：对 K1–K3 调 `ak_gesture_init`。
- `ak_input_poll()`：对每个输入：
  1. 跑 `ak_input_fsm_update()`，若有事件照常回调（**保持现状**，`down`/`up`/`long` 不变）。
  2. **仅当该输入是 K1/K2/K3**：
     - 把本次 FSM 事件喂给 `ak_gesture_feed()`；若返回 `DCLICK`，回调一次（kind=KEY, event=DCLICK）。
     - 调 `ak_gesture_tick(now_ms)`；若返回 `CLICK`，回调一次（kind=KEY, event=CLICK）。

> 判定「是不是 K1–K3」：用 `kind == AK_INPUT_KIND_KEY && (id=="k1"||"k2"||"k3")`，
> 或在 `s_inputs[]` 增加一个 `bool gesture_enabled` 标志位（更干净，推荐）。

### 4.4 `tuya_app_main.c` — 发送新事件帧

`on_input_event()` 已经对任意 `event` 调 `ak_emit_input_frame()`，后者用
`ak_input_event_str(event)` 作为 action。因此 **CLICK/DCLICK 会自动被编码成
`action:"click"|"dclick"` 的 key 帧**，无需改 `ak_emit_input_frame`。

需要检查/调整的点：

- `on_input_event()` 开头的 `#if AK_ENABLE_DISPLAY` 块里：
  `if (kind == AK_INPUT_KIND_KEY && event == AK_INPUT_EVENT_DOWN) ak_select_session_key(id);`
  —— 这是板上 LCD 选中槽位的逻辑。**保持用 `down` 不变**（板上 UI 选中与 PC 跳转是
  两回事，不要改成 click，以免板上选中也延迟到松手）。确认 CLICK/DCLICK 不会
  意外触发 settings 或其它 `down` 专属分支即可。
- `ak_settings_consume_input()`：确认它只处理 nav/center 与现有事件，
  收到 K1–K3 的 CLICK/DCLICK 时返回 false（不拦截），让其正常 emit。
- K4 的 `ak_dclick_feed(&s_ptt_dclick, ...)`：只喂 `down/long/up`，
  不受新事件影响（CLICK/DCLICK 只在 K1–K3 产生，K4 不会收到）。

### 4.5 `ak_protocol.{c,h}` — 无需改

`ak_proto_key()` 直接写入传入的 `action` 字符串，已能编码 `"click"`/`"dclick"`。
**不需要改动**（除非想加断言/文档注释）。

---

## 5. 事件去重与时序说明

一次 K1 短按（无双击）在串口上的帧序列（示意）：

```
key k1 down               <- FSM，即时
key k1 up   held_ms=40    <- FSM，即时
key k1 click held_ms=40   <- 手势层，约 400ms 后（窗口到期）
```

一次 K1 双击：

```
key k1 down ; key k1 up ; key k1 down ; key k1 up ; key k1 dclick
```
（第二次 up 时立即发 dclick；**不发** click。）

一次 K1 长按：

```
key k1 down ; key k1 long held_ms>=800 ; key k1 up held_ms>=800
```
（手势层因 `press_had_long` 不发 click/dclick；`long` 由 FSM 既有逻辑发出。）

> `held_ms` 对 click/dclick 取最近一次短按的释放时长（robustness 用途，PC 端可忽略）。

---

## 6. 固件 host 单测要求（`firmware/app/test/test_ak.c`）

新增 `test_gesture()`，覆盖：

1. **单击**：一次短按（down→up，短 hold），`feed` 返回 NONE；
   推进时钟超过 400ms 后 `tick` 返回 **CLICK 恰好一次**；再 tick 不再返回。
2. **双击**：两次短按在 400ms 窗口内，第二次 `up` 的 `feed` 返回 **DCLICK**；
   之后 `tick` **不**返回 CLICK（待定已被双击消费）。
3. **长按不产生手势**：down→long→up，`feed` 全程不返回 CLICK/DCLICK，
   后续 tick 也不返回 CLICK。
4. **窗口过期后的第二次按下算新单击**：两次短按间隔 > 400ms，
   第一次产生 CLICK，第二次独立再产生 CLICK（不是 dclick）。
5. **协议编码**：`ak_proto_key(buf, ..., "k1", "click", 40)` 与
   `"dclick"` 的输出字符串断言（照 `test_protocol()` 风格）。
6. 回归：确认 `test_short_press` / `test_long_press` / `test_dclick`（K4 用）
   仍全部通过——证明 FSM 原有输出未改变。

把 `test_gesture()` 注册进 `main()` 的测试清单。

---

## 7. Companion（PC 端）对应改动 —— 由本地负责，等新固件就绪后执行

> 本节仅为对齐说明；**本地在固件新帧可用后再实施**，避免旧固件下「短按不跳转」断档。

1. `companion/src-tauri/src/protocol.rs`：
   `KeyAction` 增加变体：
   ```rust
   #[serde(rename_all = "lowercase")]
   pub enum KeyAction { Down, Up, Long, Click, Dclick, #[serde(other)] Unknown }
   ```
   （`rename_all = "lowercase"` 会把 `Dclick` 序列化成 `"dclick"`，与协议一致。）

2. `companion/src-tauri/src/lib.rs` 的 `route_board_via_crew_api()`：
   K1–K3 的跳转/选 slot 触发点从 `KeyAction::Down` 改为 **`KeyAction::Click`**。
   `down`/`up` 不再触发跳转（但仍透传事件、仍发 `board-event` 供 UI 用）。

3. `companion/src-tauri/src/kirocrew_control.rs` 的 `route_input()`：
   K1–K3 的 `Alt+N` 兜底跳转，触发点同样从 `Down` 改为 `Click`。
   `inboundToJson` / `describe_inbound` 等需能表达新 action（透传字符串即可）。

4. 前端 `companion/src/main.tsx`：
   - 长按绑定监听已用 `action === "long"`，**无需改**。
   - 可选：`BoardEvent.kind==="key"` 的 `action` 类型补上 `"click" | "dclick"`
     以便日志/高亮逻辑识别（非必须）。

5. `dclick`：PC 端**暂不绑定任何动作**（协议能解码、帧能收到、留作将来扩展）。

6. 校验：`cargo check` + `npm run typecheck`。

---

## 8. 验收标准

固件（EC2 云编译 + 本地烧录）：

- [ ] K1–K3 短按：松手约 400ms 后在串口看到 `action:"click"` 帧；无 `click` 早于 `up`。
- [ ] K1–K3 双击：看到 `action:"dclick"`，且该次**不**出现 `click`。
- [ ] K1–K3 长按：看到 `action:"long"`，且**不**出现 `click`/`dclick`。
- [ ] K4/K5/nav/center 行为与改动前一致（语音、提交、编辑、设置页）。
- [ ] `firmware/app/test` 全部 host 单测通过（含新增 `test_gesture`）。

Companion（本地，新固件就绪后）：

- [ ] 新固件下，K1–K3 **短按才跳转**（走 click）；**长按只绑定、不跳转**。
- [ ] `cargo check` 与 `npm run typecheck` 通过。

---

## 9. 落地顺序（避免断档）

1. **先固件**：EC2 据本规格改固件 + 过 host 单测 + 烧录，确认串口能看到 click/dclick/long。
2. **再 Companion**：本地按 §7 把跳转触发点从 `down` 切到 `click`，解码 click/dclick。
3. 在此之前，Companion 维持现状（`down` 跳转 + 前端长按绑定），与现有固件自洽。

---

# 附:槽位状态颜色（左侧三块图标背景色）

> 本节是本规格文档的第二个独立功能，与 click/dclick/long 手势相互独立，可分别实施。
> 固件侧在 EC2 执行；Companion 侧状态词汇调整由本地负责。

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

1. 固件 `ak_status_color` + sidebar 着色 + 默认紫，EC2 编译烧录，灌入模拟 `display`
   帧验证五色。
2. Companion 调整 `map_hook_event` / `slot_display_status` 产出新词汇 + 解绑发 `unbound`。
3. 两侧联调：真实 Kiro Crew 跑一轮，确认 A/B/C 图标随 agent 状态变色。
