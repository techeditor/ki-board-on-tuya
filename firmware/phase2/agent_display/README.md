# P2-1 agent_display

二期功能测试:把 CodeTank 的 Agent 状态显示层(LVGL 场景 + 吉祥物动画 + 通知)移植到
v1.2 硬件(CL28CK230-18A / ST7789 / 320×240 横屏),用 **5 键 + 5 向开关模拟事件源**切换 Agent
状态和通知,为后期氛围显示打基础。

Spec:`.kiro/specs/phase2-agent-display/`(requirements / design / tasks)。

## 管线

```
5键+5向 --(ak_input)--> input_map(纯映射) --> ak_event_t 意图
   --> app_state --> ak_scene(吉祥物) / ak_notify_ui(通知条) --> LVGL --> ST7789(320×240)
```
关键:`ak_event_t` 意图层是稳定边界。P2-3 用「串口 JSONL 解析器」替换 `input_map` 产出相同意图,
即可用真实 Kiro 事件驱动同一套显示,无需改显示层。**不接 Wi-Fi/TCP/云**。

## 状态:T1–T6 完成

- T1 LVGL 上板(自研 ST7789 INVOFF 路径,rotation 90)· T2 精灵 runtime · T3 全 19 动画场景引擎
  (looping + one-shot 回落)· T4 通知 store + 轻量 LVGL 通知条 · T5 按键→意图→状态消费。
- **BUILD SUCCESS**;host 单测(通知 store + input_map)**ALL PASSED**;clang-format clean。
- flash:AP 分区用 **2.27MB / 3.5MB 上限**(全动画),余 ~1.2MB。
- **T6 真机观感已通过**:实物板(CL28CK230-18A / ST7789 320×240)上运行显示正常——帧率、
  精灵透明边缘、背光(P8)均无问题,按键切换 Agent 状态/通知符合预期。

## 按键映射(模拟事件源)

| 输入 | 动作 |
|---|---|
| 语音键 k1 短按 | 主状态循环 +1(thinking→typing→building→sweeping→confused→conducting→wizard→beacon→debugger) |
| 发送键 k2 短按 | 注入一条模拟通知 |
| 取消键 k3 短按 / 长按 | 短按 dismiss 最新 / 长按 clear 全部 |
| backspace k4 短按 | connected ↔ disconnected(断开显示 disconnected 动画) |
| 第5键 k5 短按 | one-shot(happy / alert 交替,播完回落) |
| 5向 上 / 下 | 遍历全部动画 −1 / +1(调试) |
| 5向 左 / 右 | 主状态循环 −1 / +1 |
| 5向 中心 短按 | idle ↔ sleeping |

每次事件串口打印 `[evt] key/nav 'id' up/long held=..ms -> intent=.. dir=..`。

## 构建 / 测试

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/phase2/agent_display
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py clean -f && tos.py build
tos.py flash -b 460800 && tos.py monitor -b 460800
# 纯逻辑 host 单测:
./test/run_host_tests.sh
```

## 可复用模块(供正式 firmware/app 的 Agent 区)

- `ak_scene.*`(吉祥物场景引擎)、`ak_notify_ui.*`(通知条)、`ak_event.h`(意图层)、`app_state.*`(状态消费)。
- `ak_panel.c`(ST7789 for LVGL 注册)。

## 归属

移植自 CodeTank(`ref_projects/CodeTank`),其场景/动画/RLE/通知逻辑改编自 clawd-tank(MIT,
见 `ref_projects/CodeTank/LICENSES/clawd-tank-MIT.txt`)。`app_rle_sprite.h`、`sprite_mascot_*.h`、
`app_notification.c/h` 为原样移植;`ak_scene.c`/`ak_notify_ui.c` 为适配 320×240 的重写。
