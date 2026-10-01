# v0.2 硬件归档（冻结基线）

> 冻结日期：2026-09-30
>
> 用途：项目进入 **v0.3** 重构前，把 **v0.2**（自研 T5AI-Core 版，旧 PCB 丝印 **v1.2.0**）
> 的关键硬件文档与导出物**固化归档**，防止后续 v0.3 改动覆盖。此目录内容为**只读快照**，不再更新。

## 版本对应

- 项目版本 **v0.2** = 旧硬件 PCB 丝印 **v1.2.0**（版本规范见 `docs/versioning.md`）。
- 该硬件已于 2026-09-01 通过嘉立创 EDA 网表核实（SCH↔PCB 59 元件 / 0 引脚差异，DRC 无物理违规）。

## 配置摘要（v0.2）

- 5× 机械键（CPG151101，直焊）+ WS-1002 五向摇杆
- 3× WS2812 菊花链（P4/SPI1_MOSI，经 SN74LV1T34 电平转换）
- 18P FPC LCD（CL28CK230-18A，ST7789，含 RTP 电阻触摸）
- 扩展板 Type-C + CH340C（USB-转-UART，可烧录/通信）+ LDO + S8050 自动复位
- 锂电池并联座 + 电池总开关（SK12D07VG5）
- 44P 双 1×22 排母对插开发板

## 归档内容

| 路径 | 内容 |
|---|---|
| `eda/expansion-baseboard-v1.2-change-scope.md` | v1.0→v1.2 改版范围 + 2026-09-01 网表核实记录（v0.2 最终基线） |
| `eda/pcb_review_report_2026-08-24.md` | PCB 审核报告 |
| `eda/pcb_guidelines_2026-08-23.md` | PCB 设计指南 |
| `eda/review_report_2026-08-22.md` | 原理图审核报告 |
| `exports/SCH_..._v1.2_2026-09-01.pdf` | v1.2 原理图导出（最终） |
| `exports/PCB_PCB_ExtBoard_v1.2_2026-09-01.pdf` | v1.2 PCB 导出（最终） |
| `exports/SVG_..._v1.2_2026-09-01.zip` | v1.2 SVG 导出 |
| `exports/..._v1.0_2026-08-24.*`（epro2/SCH/SVG/PNG/BOM） | v1.0 完整工程与导出（历史） |
| `pinmap/pinmap.json` | v0.2 引脚映射权威基线（已网表核实） |
| `pinmap/netlist.md` | v0.2 功能级网表 |
| `pinmap/component-notes.md` | v0.2 器件说明 |
| `pinmap/bom.md` | v0.2 BOM |

## 与活动文档的关系

- 活动的 `hardware/pinmap/*`、`hardware/eda/*` 会随 v0.3 演进；本归档保留 v0.2 定格。
- v0.3 变更见 `hardware/eda/expansion-baseboard-v0.3-change-scope.md`。
- 原始 EDA 工程（嘉立创）另存为新方案 **`Expansion Board_v0.3_lite`**；v0.2 工程保持不动。
