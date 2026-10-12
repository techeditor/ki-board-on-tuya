# v0.2 固件归档（冻结基线）

> 冻结日期：2026-10-12
>
> 用途：固件从 **v0.2 硬件**对应版本迁移到 **v0.3 硬件**对应版本前，把 v0.2 的
> `firmware/app` 源码**固化归档**，防止 v0.3 改动覆盖后无法直接对照。此目录内容为
> **只读快照**，不再更新，也不参与构建（仅供阅读/对照）。

## 版本对应

- 项目版本 **v0.2** = 旧硬件 PCB 丝印 **v1.2.0**（版本规范见 `docs/versioning.md`）。
- 本快照取自 `main` 分支（commit `5582aac`）的 `firmware/app`，固件版本 **VERSION = 0.1.12**。
- 对应硬件归档见 `hardware/archive/v0.2/`。

## 配置摘要（v0.2 固件所面向的硬件）

- 5× 机械键（K1–K5）+ WS-1002 五向摇杆（`nav` 帧：up/down/left/right/center）
- 3× WS2812 菊花链（P4 / SPI1_MOSI，经 SN74LV1T34）
- 18P FPC LCD（CL28CK230-18A，ST7789，P7 作 LCD_PWR 电源使能）
- 输入抽象 `ak_input`：`AK_INPUT_KIND_KEY` + `AK_INPUT_KIND_NAV`
- 协议 `ak_proto_nav`（dir + action + held_ms）

## 归档内容

`app/` 为 v0.2 `firmware/app` 的完整源码快照（已排除构建产物 `dist/`、`.build/`
和真实 License 头 `ak_tuya_authkey.h`，仅保留 `.example`）：

| 路径 | 内容 |
|---|---|
| `app/common/include/ak_board_io.h` | v0.2 引脚映射（5 键 + 摇杆宏，WS2812 count=3，P7=LCD_PWR） |
| `app/common/include/ak_input.h` · `app/common/src/ak_input.c` | 5 键 + 5 向摇杆的 GPIO 扫描 |
| `app/common/include/ak_protocol.h` · `app/common/src/ak_protocol.c` | 含 `ak_proto_nav` 的 JSONL v1 构造器 |
| `app/src/tuya_app_main.c` | v0.2 主程序（摇杆 center 进设置、方向导航菜单） |
| `app/common/src/ak_statuslcd.c` · `ak_display.c` | 18P LCD 驱动（P7 作 power 使能） |
| `app/config/*.config` · `app/app_default.config` · `app/VERSION` | v0.2 构建 profile 与版本（0.1.12） |
| `app/test/test_ak.c` · `run_host_tests.sh` | v0.2 host 单元测试 |
| 其余 `common/`、`include/`、`tools/` | v0.2 完整源码树 |

## 与活动固件的关系

- 活动的 `firmware/app/*` 已迁移到 **v0.3**：6 键（K1–K6）+ EC11 编码器（`encoder` 帧，
  detents 步进）+ 7× WS2812 + 12P LCD（P7 作 TE 输入）。
- v0.3 迁移详见硬件变更文档 `hardware/eda/expansion-baseboard-v0.3-change-scope.md`
  与协议 `shared/protocol/protocol.md`。
- 权威的 v0.2 固件历史仍以 `main` 分支为准；本目录是同分支内可直接对照的定格副本。
