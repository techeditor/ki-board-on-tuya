# 引脚映射与硬件连接清单（索引）

当前硬件是 **v0.3**（K1–K6 + EC11 + 12P LCD + 7×WS2812）。每份文档都带版本后缀，没有无后缀的“当前版”副本。

| 版本 | 入口 | 校验 |
|---|---|---|
| v0.3（当前） | [README-v0.3.md](README-v0.3.md)、[pinmap-v0.3.md](pinmap-v0.3.md)、[pinmap-v0.3.json](pinmap-v0.3.json) | `python3 hardware/pinmap/check_pins_v0.3.py`（按 v0.3.0 位号） |
| v0.3.1 low-profile | [pinmap-v0.3.1-low-profile.json](pinmap-v0.3.1-low-profile.json)、[netlist-v0.3.1-low-profile.md](netlist-v0.3.1-low-profile.md) | 无专用脚本；`check_pins_v0.3.py` 不适用（R5–R8 位号不同） |
| v0.3.2 standard（灯链电源开关） | [pinmap-v0.3.md](pinmap-v0.3.md) §5（P18） | 无专用脚本，仅增量说明 |

固件引脚宏：v0.3 在 `firmware/app/common/include/ak_board_io.h`。
