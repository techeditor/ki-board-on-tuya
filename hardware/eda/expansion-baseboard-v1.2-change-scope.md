# Expansion Baseboard v1.2 改版范围与待核实事项

> 状态：改版范围已确认，EDA 尚未修改
>
> 记录日期：2026-08-31
>
> 基线：`Expansion Baseboard_v1.0`
>
> 原则：本次只实施 P4、P8 和 FPC1 脚序三项修改；其余建议先核实，不纳入本次改板。

## 1. 本次改版范围

### 1.1 WS2812 数据脚由 P13 改为 P4

变更前：

```text
P13 / J1.16 -> WS2812_DIN_3V3 -> U2.A
```

变更后：

```text
P4 / J2.20 / SPI1_MOSI -> WS2812_DIN_3V3 -> U2.A
P13 / J1.16 -> NC/预留
```

实施要求：

- 原理图中断开 P13 与 `WS2812_DIN_3V3`。
- 将 P4 接到 U2 的 A 输入，U2.Y、R3、LED1 DIN 以及 LED1→LED2→LED3 数据链保持不变。
- SPI1 固定引脚组 P2/P3/P4/P5 分别为 SCK/CS/MOSI/MISO；四个引脚均不得再分配给其它功能。
- P2、P3、P5 即使没有外部负载，也保留给 SPI1，避免 TKL 初始化完整 SPI1 引脚组时产生复用冲突。
- 固件改用 SPI1 和 TuyaOpen WS2812 官方等效编码时序，不再使用 P13 GPIO bit-bang。

验收：

- P13 与 U2.A 不导通。
- P4 与 U2.A 导通，且对地、3.3V、5V 无短路。
- U2.Y、R3 两端和 LED1 DIN 可测得 WS2812 数据波形。

### 1.2 LCD 背光控制由 P5 改为 P8

变更前：

```text
P5 / J2.19 -> LCD_BL -> R2 -> Q1 基极
```

变更后：

```text
P8 / J2.16 / PWM2 -> LCD_BL -> R2 -> Q1 基极
P5 / J2.19 -> SPI1_MISO 保留
```

实施要求：

- 原理图中断开 P5 与 `LCD_BL`。
- 将 P8 接入原 `LCD_BL` 网络；R2、Q1、R1 和 LCD LEDK 低边开关路径保持不变。
- 背光仍为高电平有效。
- 固件中的 `AK_LCD_BACKLIGHT_PIN` 改为 `TUYA_GPIO_NUM_8`。
- P8 可先按普通 GPIO 开关使用；需要调光时再启用 PWM2。

验收：

- P5 与 Q1 基极电阻输入端不导通。
- P8 与 `LCD_BL`/R2 输入端导通。
- 上电默认背光状态可控，GPIO 高/低能可靠控制背光开关。

### 1.3 修正 FPC1 底层放置造成的脚序镜像

已确认 v1.0 的 FPC1 在底层放置后物理脚序发生 Pin1↔Pin18 镜像，实物依赖反向插入排线才能工作。v1.2 必须修正 footprint/焊盘编号，使排线按连接器正常方向插入。

修正后的逻辑脚序：

| FPC 引脚 | 信号 |
|---|---|
| 1 | GND |
| 2 | LCD_RST |
| 3 | LCD_SCK |
| 4 | LCD_DC |
| 5 | LCD_CS |
| 6 | LCD_MOSI |
| 7 | NC |
| 8 | GND |
| 9 | VDD_3V3 |
| 10 | LEDA / VDD_3V3 |
| 11–14 | LCD_LEDK |
| 15–18 | 触摸预留/NC |

实施要求：

- 修正底层 FPC footprint 的焊盘编号，而不是仅旋转丝印或交换网络标签。
- 在原理图符号、PCB 焊盘、连接器 Pin1 标记和排线触点方向之间逐项核对。
- v1.2 不再允许以反插排线作为正常使用方式。

验收：

- PCB 底视图和实物连接器上均可明确识别 Pin1。
- 正常方向插入排线后，FPC1.1 对 GND、FPC1.2 对 LCD_RST、FPC1.9 对 3.3V。
- 上电前逐脚做连续性与相邻脚短路检查。

## 2. 本次保持不变

- LCD SPI0：P14=SCK、P15=CS、P16=MOSI、P17=DC。
- LCD RST：P6。
- WS2812 的 U2、R3、LED1–LED3 菊花链、5V 供电和每颗 100nF 去耦。
- 五个机械键与五向摇杆的 GPIO 分配。
- CH340C、Type-C、自动复位和 UART0 电路。
- 电池座、电池开关和电源指示灯电路。
- PCB 外形、安装孔、母排位置及其它未涉及网络。

## 3. 待核实事项：本次不修改

以下项目保留为下一轮评审输入。没有完成实测或器件核对前，不直接写入本次 EDA 变更。

| 项目 | 当前状态 | 后续核实内容 |
|---|---|---|
| U2 去耦 | 待核实 | 确认 U2 VCC 附近是否已有独立 100nF；若没有，再评估增加 |
| WS2812 储能 | 待核实 | 测量系统 5V 压降后，再决定是否在灯链入口增加 10–47µF |
| WS2812 测试点 | 待核实 | 评估增加 P4、U2.Y、LED1 DIN/DOUT 和系统 5V 测试点 |
| U2 器件替换 | 待核实 | SN74LV1T34 官方支持 reduced-threshold 上行转换；先测 U2.A/U2.Y 波形，再决定是否更换 |
| 背光基极下拉 | 待核实 | 观察启动闪烁后，再决定是否给 Q1 基极增加 47k–100k 下拉 |
| LCD_PWR/P7 | 待核实 | v1.0 原理图中 LCD VCC 实际直连 3.3V，P7 未形成文档所述电源开关；后续决定删除该定义或增加负载开关 |
| 电池开关额定 | 待核实 | SK12D07VG5 官方额定 0.3A，需测整机峰值电流后决定是否换 ≥1A 型号 |
| 双电池座并联 | 待核实 | 评估删除一个入口、增加互锁或电源 ORing，不能仅依赖用户提示 |
| 电源线宽 | 待核实 | 根据实测峰值电流决定 BAT/5V 是否由 0.508mm 加宽到 0.8–1.0mm |
| USB ESD 位置 | 待核实 | 下一版布局时评估将 ESD 器件进一步靠近 Type-C 连接器 |
| 丝印与方向 | 待核实 | 增加 WS2812 Pin1、数据流方向、UART ONLY、单电池提示等丝印 |

## 4. 文档与 BOM 一致性：本次仅记录

现有资料中存在位号和说明不一致，后续应统一，但不属于本次三项 EDA 修改：

- `gen_bom.mjs` 中 R2/R3、Q1/Q2 的功能备注与当前 EDA 实际位号不一致。
- `gen_bom.mjs` 将 SK12D07VG5 标为 0.5A，而器件资料记录为 0.3A。
- `component-notes.md` 仍引用旧 WS2812 物料；v1.0 实际为 XL-0807RGBC-2812B / C3646929。
- `netlist.md` 对单电源 SN74LV1T34 使用了“VCC-A/VCC-B”表述，应改为 U2.VCC=5V、A 输入为 3.3V。
- PCB 审核报告中关于 WS2812 DIN 串阻的旧结论存在前后不一致；EDA v1.0 实际已有 R3。

## 5. 改板后检查清单

- [ ] 原理图网络仅包含本文件第 1 节的三项变更。
- [ ] P4/P8/P5/P13 网络名称和排针位置正确。
- [ ] SPI1 的 P2/P3/P4/P5 无其它功能冲突。
- [ ] FPC1 正常插线方向与 Pin1 标记一致。
- [ ] 更新 PCB 后重新铺铜并执行 ERC/DRC。
- [ ] 更新 `pinmap.json`、`pinmap.md`、公共固件引脚宏和冲突检查。
- [ ] 单独验证 LCD：复位、背光、彩条与正常方向插线。
- [ ] 单独验证 WS2812：全灭、单灯三色、全白和菊花链顺序。

## 6. 参考资料

- `hardware/pinmap/pinmap.md`
- `hardware/pinmap/netlist.md`
- `hardware/pinmap/component-notes.md`
- `hardware/eda/pcb_review_report_2026-08-24.md`
- `hardware/exports/SCH_Expansion Baseboard_v1.0_2026-08-24.pdf`
- T5AI SPI1 固定引脚组：P2/P3/P4/P5
