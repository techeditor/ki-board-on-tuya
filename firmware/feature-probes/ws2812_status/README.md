# FP-WS2812 · WS2812 Chat 状态灯 probe

把交互设计 §6 的 **LED1–LED3 → 三个 Agent 槽位 chat 状态** 灯效做成可上板 / 可单测的原型,
验证后合并进产品固件 `firmware/app/`。Spec:`.kiro/specs/feature-probes-ws2812-status/`。

硬件:扩展底板 v1.2,3× WS2812 菊花链,数据脚 **P4 = SPI1_MOSI**,经 SN74LV1T34 电平转换,
官方 SPI 驱动 `tdd_pixel_ws2812`。引脚宏复用 `firmware/app/common/include/ak_board_io.h`。

## 进度(见 spec tasks.md)

- [x] **A1 — 脚手架 + `ak_leds` 驱动 + 上板自检** ← 当前
  - `ak_leds.h/.c`:官方 SPI WS2812 驱动封装(register/open/set/show)+ 亮度上限 `AK_LEDS_MAX_LEVEL=16`
    + init 失败降级(灯不亮但不阻塞)。
  - `tuya_app_main.c`:init → 循环自检 `all_off → led0_red → led1_green → led2_blue → all_white`,
    每步串口打一行 JSON,验证 3 颗独立可控 + 颜色顺序正确。
- [ ] A2 — 状态机 `ak_led_state` + 纯函数 `ak_led_render` + host 单测
- [ ] A3 — tick 线程接线(状态机→ak_leds)
- [ ] A4 — 事件源(按键映射 + `display` 帧解析)+ host 单测
- [ ] A5 — 真机逐条验证 §6 状态表
- [ ] A6 — 收尾

## 亮度与电流(重要)

`AK_LEDS_MAX_LEVEL=16` 是 bring-up 已验证的安全档:3× 全白峰值电流须落在电池开关
SK12D07VG5 的 0.3A 额定内。**调高前必须重新核算电流**(硬件待办)。见 spec design D3。

## 构建

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/ws2812_status
tos.py clean -f && tos.py build      # 改 .config 后必须 clean
```

烧录后接串口(UART0 / 115200,或调试 Uart1 / 460800 看日志),应看到:

```
{"v":1,"type":"probe","payload":{"name":"ws2812_status","status":"ready",...}}
{"v":1,"type":"probe","payload":{"name":"ws2812_status","selftest":"led0_red"}}
...
```

肉眼预期:LED0 红 → LED1 绿 → LED2 蓝 → 三灯白,循环。确认颜色顺序与位号一致
(若 R/G/B 顺序不对,调 `ak_leds.c` 的 `line_seq`)。

## host 单测

A2 起,纯逻辑模块(`ak_led_state` render / `led_input_map` / `led_frame_parse`)带 host 单测:

```bash
cd test && ./run_host_tests.sh
```
