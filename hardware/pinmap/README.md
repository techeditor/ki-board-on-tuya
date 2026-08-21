# 引脚映射与硬件连接清单（Task 2）

路线：**T5AI-Core 成品开发板** 插到**自制扩展底板 PCB**（44P 排针）。
扩展底板只承载 5键 + 5向摇杆 + LCD FPC 座 + 保护/去耦；电源/USB/音频/MIC 由开发板负担。

## 文件索引

| 文件 | 内容 |
|------|------|
| [pinmap.md](pinmap.md) | 能力映射、已占用引脚、LCD 接线、输入分配、电路细节、待确认项 |
| [pinmap.json](pinmap.json) | 机器可读引脚分配（供校验/网表） |
| [check_pins.py](check_pins.py) | 引脚冲突校验脚本（已通过，无冲突） |
| [netlist.md](netlist.md) | 功能级网表（供 jlceda-mcp / 人工绘图） |
| [bom.md](bom.md) | 元器件清单，对照库存标记采购项 |

## 运行校验

```bash
cd hardware/pinmap
python check_pins.py   # exit 0 = 无冲突
```

## 核心结论

- **输入分配（落在排针 J2 实际引出脚）**：KEY1-5 = P24/P25/P26/P27/P23；
  NAV 上/下/左/右/中 = P32/P20/P21/P22/P30。全内部上拉、低有效。
- **LCD**：沿用官方 ST7789 接线（P14/16/15/17/6/5/7），固件仅改分辨率 240→320；
  18P FPC 逐脚定义已确认（4线SPI+电阻触摸，见 pinmap.md §3）。
- **44P 排针** = 2× 1×22 单排（J1+J2），逐脚映射见 pinmap.md §8。
- **采购**：T5AI-Core 开发板 ×1、18P 0.5mm FPC 座、2 条 1×22 母排（详见 bom.md）。
- **三个原待确认项已全部由资料核实**（44P 位号 / 18P FPC / 摇杆脚序）。
  剩余实机项：SPI 阻尼/背光限流阻值、方向↔上下左右标定。
