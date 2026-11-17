# 引脚映射与硬件连接清单（索引）

当前硬件是 **v0.3**（K1–K6 + EC11 + 12P LCD + 7×WS2812）。每份文档都带版本后缀，没有无后缀的“当前版”副本。

| 版本 | 入口 | 校验 |
|---|---|---|
| v0.3（当前） | [README-v0.3.md](README-v0.3.md)、[pinmap-v0.3.md](pinmap-v0.3.md)、[pinmap-v0.3.json](pinmap-v0.3.json) | `python3 hardware/pinmap/check_pins_v0.3.py` |
| v0.3.1 low-profile | [pinmap-v0.3.1-low-profile.json](pinmap-v0.3.1-low-profile.json)、[netlist-v0.3.1-low-profile.md](netlist-v0.3.1-low-profile.md) | 同上 |
| v0.2（归档） | [README-v0.2.md](README-v0.2.md)、[pinmap-v0.2.md](pinmap-v0.2.md)、[pinmap-v0.2.json](pinmap-v0.2.json) | `python3 hardware/pinmap/check_pins_v0.2.py` |

固件引脚宏：v0.3 在 `firmware/app/common/include/ak_board_io.h`。
