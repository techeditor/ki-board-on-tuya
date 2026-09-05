# Phase-2 功能验证测试 (Phase-2 Feature Tests)

Bringup(一期)验证的是**硬件基础功能**(每个外设单点通不通:LCD/WS2812/按键/串口/无线自检)。
Phase-2(二期)在硬件已通的基础上,**为最终方案的关键功能子系统各做一个可运行的验证程序**——
每个 test 是一个自包含的 TuyaOpen app(与 bringup 相同的目录约定),聚焦一条功能链路,
产出可复用到正式 `firmware/app/` 的模块。

> 一期 = "这块屏能亮吗";二期 = "这块屏能按产品的样子显示 Agent 状态吗"。

## 目录约定 (与 bringup 一致)

```
firmware/phase2/<test_name>/
├── src/tuya_app_main.c      # 入口
├── config/TUYA_T5AI_CORE.config
├── app_default.config
├── CMakeLists.txt
└── README.md                # 该 test 的目的/操作/预期
```
共享代码放 `firmware/phase2/common/`(如需),spec 放 `.kiro/specs/phase2-*/`。

## 测试路线图 (Roadmap)

| ID | 名称 | 验证的功能链路 | 状态 | Spec |
|---|---|---|---|---|
| **P2-1** | **agent-display** | LVGL 场景 + 吉祥物动画 + 通知 UI,**按键模拟切换 Agent 状态/通知**(移植自 CodeTank);为氛围显示打基础 | ✅ T1–T6 完成(含真机观感) | `.kiro/specs/phase2-agent-display/` |
| P2-2 | voice-dictation | 板载 Mic → 云 ASR → `AI_USER_EVT_ASR_OK` 取文字(分句流式验证) | ⏳ 规划 | — |
| P2-3 | companion-link | USB 串口 JSONL v1 ↔ Tauri Companion:真实 Kiro 事件驱动显示 | ⏳ 规划 | — |
| P2-4 | ble-gatt | BLE 自定义 GATT 传输(按键/文字/状态)验证 | ⏳ 规划 | — |
| P2-5 | ui-3zone | 三区产品 UI(status bar / Agent zone / key hints)整合 | ⏳ 规划 | — |

> 路线图按需增补。P2-1 是本轮开工项;P2-3 之后 P2-1 的场景/通知模块直接接真实事件源即成为正式 Agent 区。

## 构建

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/phase2/<test_name>
tos.py config choice   # TUYA_T5AI_CORE.config
tos.py build           # 改 .config 后先 tos.py clean -f
```
纯逻辑模块(通知 store、输入映射)带 host 单测,沿用 bringup/app 的 `run_host_tests.sh` 模式。
