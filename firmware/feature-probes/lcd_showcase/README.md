# FP-lcd-showcase · 富信息 LCD 界面示例

一个**纯显示示例** probe：把产品的**信息密集状态界面**画出来（顶栏 / K1–K3 三槽位 /
上下文主区 / 底栏方向义），**吉祥物降为右下角点缀**，用 **5 键 + 5 向开关模拟数据源**在
若干示例界面态间切换。它复用 FP-1 `agent_display` 已真机验证的显示底座，新增一层富信息 HUD，
作为后期 FP-4/FP-7 真实数据驱动时的**界面模板与观感基准**。

Spec：`.kiro/specs/feature-probes-lcd-showcase/`（requirements / design / tasks）。

## 管线

```
5键+5向 --(ak_input, 复用)--> showcase_map(纯映射) --> showcase_intent_t
   --> showcase_state --> showcase_view_t 快照
        --> ak_hud(富信息布局) + ak_scene(角落吉祥物, 复用) + ak_notify_ui(通知条, 复用)
        --> LVGL --> ST7789(320×240 横屏 rotation 270)
```

关键：`showcase_view_t` 是「状态 → 绘制」之间的**稳定快照边界**。FP-4 用真实 Kiro 数据
（串口 JSONL）填同一个 `showcase_view_t`，即可驱动同一套 HUD，无需改绘制层。**不接
Wi-Fi / BLE / 云 / 麦克风**——按键只用来演示各界面态长什么样。

## 屏幕布局（320×240 横屏）

```
┌───────────────────────────────────────────────────────────┐  y=0
│  KiroCrew  ·  PTT  ·  eff:High         ON            v0.1.0 │  顶栏 h=22
├──────────────┬──────────────┬─────────────────────────────┤  y=24
│ >K1 Kilo     │  K2  Sonnet  │  K3  -                       │  三槽位卡片
│  ● Run       │  ○ Idle      │    Empty                     │  h=62
│  kiro/crew   │  keyboard    │                              │
├──────────────┴──────────────┴─────────────────────────────┤  y=88
│  Context                                                    │
│  > building firmware...                        ╭────────╮   │  上下文主区
│  last: "add rate limiting to upload"           │  Kiro  │   │  h=112
│  [ listening ▁▃ ]  (录音态)                     │  幽灵  │   │  右下角吉祥物
│  ⚠ press ▶ to approve  (待确认态)               ╰────────╯   │
├─────────────────────────────────────────────────────────────┤  y=202
│ ◀ Back  ▶ Approve  ● Details   K4:PTT  K5:Send              │  底栏提示
└─────────────────────────────────────────────────────────────┘  底部：通知条(复用,有通知才浮现)
```

- 顶栏 / 槽位卡片 / 上下文文本 / 底栏提示均为 LVGL label + 圆角矩形（Montserrat 14/28）。
  配色沿用 FP-1 深色底（0x101018）+ Kiro 紫 0xC8B4FF + 青 + 绿/琥珀/红语义色（同 ki-board）。
- 吉祥物（`ak_scene`）对齐**右下角**（`LV_ALIGN_BOTTOM_RIGHT`，不放大），随聚焦槽状态切换动画。
- 通知条（`ak_notify_ui`）在屏幕最底部，有通知时上浮，覆盖底栏提示一小条；无则隐藏。

## 按键映射（模拟数据源，非最终产品键位）

| 输入 | 动作 |
|---|---|
| K1 / K2 / K3 短按 | 聚焦槽位 1 / 2 / 3（当前槽高亮） |
| K4 短按 | 切换 PTT 录音态 on/off（上下文区显示 listening） |
| K5 短按 | 聚焦槽状态循环 Idle → Run → Confirm → Idle |
| 5 向 左 / 右 | 上一 / 下一**示例场景**（6 个态整体遍历） |
| 5 向 中心 短按 | 注入 / 清除示例通知 |
| 5 向 上 / 下 | 切换连接态 ON / OFF（演示断连） |

> 均为短按（`UP` 且 held < 800ms）；DOWN/LONG 不产出意图。
> 每次事件串口打印 `[showcase] key/nav 'id' -> intent=... scene=.. focus=.. rec=.. conn=..`。

## 6 个示例场景（5 向左右整体遍历）

| # | 场景 | view 关键字段 | 吉祥物 |
|---|---|---|---|
| 0 | 三槽常态 | slots=[Run, Idle, Empty]，focus=0 | typing |
| 1 | 聚焦 K2 | focus=1（slot1 Idle） | idle |
| 2 | PTT 录音 | recording=true，context="listening…" | thinking |
| 3 | 待确认 | awaiting_confirm=true，slot0=Confirm | alert（one-shot 播完回落） |
| 4 | 通知 | 注入一条 "Build finished" | idle + 通知条浮现 |
| 5 | 断连 | connected=false，顶栏 OFF | disconnected |

吉祥物动画由 `showcase_state_anim_for_view()` 从当前 view 纯函数推导：断连→disconnected、
待确认→alert、录音→thinking、聚焦槽 Run→typing / Confirm→alert / Error→confused / 其余→idle。

## 构建 / 测试

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/lcd_showcase
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py clean -f && tos.py build
# 纯逻辑 host 单测（showcase_map + showcase_state，无需 SDK/硬件）：
./test/run_host_tests.sh
```

真机烧录（扩展板 CH340C = COM6）：

```powershell
.\flash.ps1 -Port COM6 -App firmware/feature-probes/lcd_showcase -Monitor
```

## 状态

- **BUILD SUCCESS**（AP 分区 **2.28MB / 3.5MB**，余 ~1.4MB）。
- host 单测 **ALL PASSED**（按键映射 + 6 场景环回 + 状态循环 + confirm one-shot + anim 映射）。
- `clang-format` clean（用 TuyaOpen 根 `.clang-format`）。
- **真机观感待用户在扩展板（COM6）验收**——云端主机只保证编译通过 + 逻辑正确。

## 复用 vs 新增

| 模块 | 来源 | 处理 |
|---|---|---|
| `ak_panel.c/.h` · `ak_scene.c/.h` · `ak_notify_ui.c/.h` · `app_notification.c/h` · `app_rle_sprite.h` · `assets/generated/*` | FP-1（移植自 CodeTank） | 复用（`ak_scene` 加了 `ak_scene_set_align()` 让吉祥物落角落） |
| `ak_input.c/.h` · `ak_input_fsm.c/.h` | `firmware/app/common` | 复用（5 键 + 5 向扫描） |
| `ak_hud.c/.h` | **新增** | 富信息 HUD 布局 |
| `showcase_state.c/.h` | **新增** | 示例状态机 + `showcase_view_t` + 6 场景 + anim 映射 |
| `showcase_map.c/.h` | **新增** | 按键 → 示例意图（纯函数，host 可测） |
| `tuya_app_main.c` | **新增** | 入口，装配 hooks |

## 归属

显示层的吉祥物场景 / 动画 / RLE / 通知逻辑移植自 CodeTank（`ref_projects/CodeTank`，其精灵 /
场景改编自 clawd-tank，MIT）。富信息 HUD 布局参考 ki-board `ui_render.cpp` 观感（3 槽位卡片 +
状态栏 + 底栏提示），按本项目 320×240 横屏与 `docs/agentic-keyboard-interaction-design.md`
重写，不复制其代码。

## Kiro 主题吉祥物（换肤）

当前角落吉祥物仍是 CodeTank 浣熊精灵。换成 Kiro 幽灵的资产管线脚手架在
`tools/kiro_mascot/`，逐步手工出图操作手册见内部文档。
