# FP-WS2812 · WS2812 Chat 状态灯 · Tasks

> 增量推进,每个 task 结束应能 `tos.py build` 通过(或 host 单测通过)。风险优先。
> 勾选规则:`[ ]` 未开始 / `[~]` 进行中 / `[x]` 完成。
> 两阶段:**A = feature-probe 原型**(可上板/可单测),**B = 合并进 `firmware/app/`**。

## 阶段 A — Probe 原型

- [x] **A1 — 脚手架 + `ak_leds` 驱动上板(风险优先, R1/R4/D1)** ✅ 2026-09-14(EC2 build)
  - 建 `firmware/feature-probes/ws2812_status/{src,include,config}` + `app_default.config`
    (`CONFIG_ENABLE_LEDS_PIXEL=y`)+ `config/TUYA_T5AI_CORE.config` + `CMakeLists.txt` + `README.md`
  - 写 `ak_leds.c/h`:封装 bring-up 已验证的 register/open/set/show 序列 + `AK_LEDS_MAX_LEVEL=16` 上限 + 失败降级(`ready()`,set/show 变 no-op)
  - `tuya_app_main.c`:init → 循环自检 `all_off→led0_red→led1_green→led2_blue→all_white`,每步串口打 JSON
  - **BUILD SUCCESS**(`ws2812_status_QIO_0.1.0.bin`,ap size 811008 / limit 3670016)+ clang-format clean
  - 引脚复用 `firmware/app/common/include/ak_board_io.h`;UART 复用 `firmware/bringup/common`
  - ⏳ 真机三灯颜色/顺序核对待硬件在手时执行(A5 一并做)

- [ ] **A2 — 状态机 `ak_led_state` + `ak_led_render` 纯函数 + host 单测(R2, 核心逻辑)**
  - 写 `ak_led_state.c/h`:数据模型(3 槽/选中/覆盖)+ `init/set_slot/set_selected/set_override`
    + 纯函数 `ak_led_render(st, tick, rgb[3])`(基色 × 动态,见 design §4.3)
  - Complete 回落计时(`complete_since_tick` + timeout,design §4.3/D5)
  - `test/`:host 单测覆盖各状态基色、呼吸/双闪/慢闪关键相位、断线覆盖、选中叠白闪、Complete 超时
  - **DoD**:host 单测全绿(纯逻辑,不依赖硬件)

- [ ] **A3 — tick 线程接线:状态机 → `ak_leds`(集成 A1+A2)**
  - `tuya_app_main` tick 线程(~50ms):`ak_led_render` → 逐 slot `ak_leds_set` → `ak_leds_show`
  - 默认三灯 UNBOUND(熄灭);预置一组静态测试状态验证各槽独立
  - **DoD**:`tos.py build` 通过;真机各槽能显示 design §4.3 定义的静态颜色

- [ ] **A4 — 事件源:按键映射 + `display` 帧解析(R3)**
  - `led_input_map.c/h`(纯逻辑,可单测):§6 按键→ `ak_led_intent_t`,复用 `ak_input`/`ak_input_fsm`
  - `led_frame_parse.c/h`(纯逻辑,可单测):串口 `display` JSONL → intent,`chat_slot` 1..3→slot,
    越界/缺字段拒绝
  - `on_intent` 落地到 `ak_led_state_*` 并打印 `[led] slot=.. state=.. selected=.. override=..`
  - `test/`:host 单测覆盖 input_map 全表 + frame_parse(合法/越界/缺字段)
  - **DoD**:`tos.py build` 通过;host 单测全绿

- [ ] **A5 — 真机逐条验证 §6 状态表**
  - 上板:按键把各槽切到 IDLE/RUNNING/TOOL/COMPLETE/APPROVAL/ERROR,肉眼核对颜色/动态
  - 验证:Complete 绿超时回落白;选中槽叠白闪;中心键触发断线三灯紫慢闪并可恢复
  - (可选)companion/串口工具发 `display` 帧,验证帧驱动路径
  - **DoD**:全部与 §6 表一致;`[led]` 日志匹配;记录到 probe README 与 TEST_RESULTS

- [ ] **A6 — Probe 收尾**
  - README(操作/按键表/预期/构建测试命令)+ `firmware/feature-probes/README.md` 路线图加一行
  - 标注可复用模块:`ak_leds.*` / `ak_led_state.*` 供阶段 B 迁入 `firmware/app/common/`

## 阶段 B — 合并进产品固件

- [ ] **B1 — 迁移可复用模块到 `firmware/app/common/`**
  - `ak_leds.*` + `ak_led_state.*` 落 `firmware/app/common/{src,include}`,加入 `firmware/app/CMakeLists.txt`
  - `firmware/app/Kconfig` 加 WS2812 状态灯开关(默认开),`app_default.config` 置 `CONFIG_ENABLE_LEDS_PIXEL=y`
  - **DoD**:`firmware/app` `tos.py build` 通过(未接线,仅编入)

- [ ] **B2 — 接线:`display` 帧 → 状态机 + tick + 断线覆盖(R5.2/R5.3)**
  - 在 `tuya_app_main.c` 处理 `display` 帧的既有位置调用 `ak_led_state_set_slot(...)`(§5 映射)
  - 把 render tick 挂到已有软 timer / 输入线程(不新增线程冲突,D4)
  - 断线覆盖接产品固件已有连接状态(心跳/`system_info` 丢失)→ `set_override(DISCONNECTED)`
  - init 失败按 R1.4 降级,不阻塞
  - **DoD**:`firmware/app` `tos.py build` 通过;`firmware/app/test/` 新增/迁入 render+映射单测全绿

- [ ] **B3 — 真机端到端 + 文档**
  - companion 发 `display` 帧,三灯随对应槽位 chat 状态实时变化;拔线/断连三灯紫慢闪
  - 更新 `docs/internal/dev-implementation-steps.md` 路线图、`firmware/app/README.md`
  - (若阶段 A/B 发现协议需扩 `display.state` 枚举 → 记入 D2 决策并同步 `shared/protocol/protocol.md`)
  - **DoD**:端到端演示通过;文档更新

## 依赖顺序
A1 → A2 →(A3 集成)→ A4 → A5 → A6 → B1 → B2 → B3
A1 是硬门槛(驱动封装不通则整体要改);A2 是纯逻辑核心,可与 A1 并行起草但集成在 A3。
阶段 B 在 A5 真机验证通过后再开(保险优先原则)。

## 构建/测试命令
```bash
# 编译(EC2 云编译)
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/ws2812_status
tos.py clean -f && tos.py build      # 改 .config 后必须 clean

# host 单测(纯逻辑:render + input_map + frame_parse)
cd test && ./run_host_tests.sh
```
