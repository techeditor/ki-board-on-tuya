# 网表（功能级）— 扩展底板

> 供 jlceda-mcp 或人工绘制原理图的连接清单。
> 44P 排针（开发板 J1/J2 两排）与 18P FPC、摇杆脚序**逐脚已确认**（见 pinmap.md §3/§4/§8）。
> 本网表元件位号：**H1/H2** = 扩展板上插开发板的两个 1×22 母排座（对应开发板 J1/J2）。

## 元件清单（Designator）

| 位号 | 元件 | 说明 |
|------|------|------|
| H1 | 1×22 2.54mm 母排座 | 插开发板 **J1 排**（3V3/5V/GND + LCD SPI + P13 等） |
| H2 | 1×22 2.54mm 母排座 | 插开发板 **J2 排**（输入 GPIO + UART0 + T5_RST + DP/DN 等） |
| J_LCD | 18P 0.5mm FPC 座（国连 C51901319） | 接 CL28CK230-18A LCD |
| SW1..SW5 | 机械键 CPG151101D06 茶轴（直焊） | 5× 机械按键 |
| SW6 | 5向摇杆 WS-1002 | 4向+中心 |
| J_USBC | Type-C 母座（16P） | 扩展板 USB 口（烧录+通信） |
| U_USB | CH340C（SOP-16） | USB-转-UART 桥 |
| U_LDO | HE9073A33M5R（SOT-23-5,3.3V,库存） | Type-C VBUS 5V → 3.3V 给 CH340C |
| Q_RST | S8050（SOT-23，NPN） | 自动复位三极管 |
| U_LS | SN74LV1T34（SOT-23-5） | WS2812 电平转换 3.3→5V |
| LED1..LED3 | WS2812 RGB LED | 菊花链 |
| J_BAT | JST PH 1.25mm 2P 座 | 锂电池并联座 |
| SW_BAT | 拨动开关 SK12D07VG5（C431548，库存） | 电池总开关，串于 BAT+ |
| R1..R2 | 22Ω 0402（可选） | LCD SPI SCK/MOSI 阻尼 |
| R_TX | 2kΩ 0402（库存 C60488） | CH340C TX→T5 RX 隔离 |
| R_CC1/R_CC2 | 5.1kΩ 0402 | Type-C CC 下拉（UFP/设备角色） |
| R_DTR | 4.7kΩ 0402（=开发板 R22） | CH340C DTR → Q_RST 基极 串阻 |
| R_RST | 100Ω 0402（=开发板 R14） | Q_RST 集电极 → T5_RST 串阻 |
| R_RST_PU | 10kΩ 0402（=开发板 R47，可选） | T5_RST 上拉到 3V3（开发板已有，扩展板可不重复） |
| R_LED | 100-330Ω 0402 | WS2812 DIN 串阻 |
| C_USB1 | 100nF | CH340C VCC 去耦 |
| C_USB2 | 1µF | CH340C VCC 储能 |
| C_LED1..3 | 100nF | 每颗 WS2812 去耦 |
| C1..C2 | 1µF | LCD FPC 电源去耦 |
| C3..C4 | 100nF | LCD FPC 电源去耦 |
| D_ESD* | LESD5D5.0 / ESD 阵列 | USB D+/D-、可触摸接口 ESD |

## 网络（Net）连接

### LCD SPI（信号来自 J1 上开发板对应引脚；J2:FPC 为 18P FPC 脚号）
| Net | 从（T5引脚/排针） | 到（J2 FPC 脚） | 备注 |
|-----|------------------|----------------|------|
| `LCD_SCK`  | P14 / J1.14 | FPC.2 (SCL)（经 R1 22Ω）  | 48MHz |
| `LCD_MOSI` | P16 / J1.13 | FPC.5 (SDA)（经 R2 22Ω） | |
| `LCD_CS`   | P15 / J1.15 | FPC.4 (CS) | |
| `LCD_DC`   | P17 / J1.12 | FPC.3 (RS) | |
| `LCD_RST`  | P6  / J2.18 | FPC.1 (RST) | |
| `LCD_BL`   | P5  / J2.19 | FPC.9-13 (LEDA + LEDK1-4 经限流 R) | 高有效 |
| `LCD_PWR`  | P7  / J2.17 | FPC.8 (VCC) | 高有效，屏 3.3V 使能 |
| `GND`      | GND | FPC.7 | |
| （预留触摸） | — | FPC.14-18 (XL/YU/XR/YD) | 一期不接 |

### 输入（低有效，公共端 GND；上拉用 T5 内部上拉）
| Net | 从（J1 / T5引脚） | 到 |
|-----|------------------|-----|
| `KEY1` | J2.13 (P24) | SW1 一端（另一端 GND） |
| `KEY2` | J2.8 (P25) | SW2 |
| `KEY3` | J2.9 (P26) | SW3 |
| `KEY4` | J2.12 (P27) | SW4 |
| `KEY5` | J2.7 (P23) | SW5 |
| `NAV_UP`（摇杆①A）     | J2.14 (P32) | SW6-① |
| `NAV_DOWN`（摇杆③B）   | J2.4 (P20)  | SW6-③ |
| `NAV_LEFT`（摇杆⑥C）   | J2.5 (P21)  | SW6-⑥ |
| `NAV_RIGHT`（摇杆④D）  | J2.6 (P22)  | SW6-④ |
| `NAV_CENTER`（摇杆②）  | J2.11 (P30) | SW6-② |
| `GND`（摇杆⑤Com）      | J1/J2 GND   | SW6-⑤ 公共端 |

### 电源
| Net | 连接 |
|-----|------|
| `3V3` | H1(3V3) → LCD FPC VCC(经P7使能)、输入上拉参考(内部)、SN74LV1T34 VCC-A侧 |
| `5V`  | H1(5V) → WS2812 VDD、SN74LV1T34 VCC-B侧 |
| `GND` | H1/H2(GND) → 所有器件 GND |
| `CH340_3V3` | U_LDO OUT → CH340C VCC（**独立，来自 Type-C VBUS→LDO，不接 3V3/5V 系统电**） |

> ⚠️ **Type-C VBUS 不接系统 5V**（只经 LDO 供 CH340C）。系统 5V 仅由开发板经排针供给。

### 扩展板 Type-C + CH340C（USB-转-UART，烧录+通信）
| Net | 从 | 到 | 备注 |
|-----|----|----|------|
| USB D+ | J_USBC(D+) | U_USB(CH340C) UD+ | 串 ESD |
| USB D- | J_USBC(D-) | U_USB UD- | 串 ESD |
| `VBUS` | J_USBC(VBUS 5V) | U_LDO IN（**不接系统/排针5V**） | 仅供 CH340C |
| `CH340_3V3` | U_LDO OUT(3.3V) | U_USB(CH340C) VCC | LDO 降压，CH340C IO=3.3V 匹配 T5 |
| Type-C CC1 | J_USBC(CC1) | R_CC1 5.1kΩ → GND | UFP/设备角色 |
| Type-C CC2 | J_USBC(CC2) | R_CC2 5.1kΩ → GND | |
| `UART0_RX`(T5) | U_USB TXD → **R_TX 2kΩ** → H2(P10 / J2 的 RXD 脚) | | CH340C 发 → T5 收；串阻隔离 |
| `UART0_TX`(T5) | H2(P11 / J2 的 TXD 脚) → U_USB RXD | | T5 发 → CH340C 收 |
| `RTS` | U_USB RTS → Q_RST(S8050) 基极 | 直连 | RTS 控制复位（开发板方案） |
| `DTR` | U_USB DTR → R_DTR(4.7k) → Q_RST 基极 | | DTR 经 4.7k 汇入基极 |
| `T5_RST` | Q_RST 集电极 → R_RST(100Ω) → H2(J2.2 T5_RST)；发射极→GND | | RTS/DTR 组合拉低复位进下载 |
| CH340C 去耦 | C_USB1 100nF + C_USB2 1µF @VCC(3.3V)；**V3 脚与 VCC 短接**(3.3V 供电时) | GND | |

> **复位电路已按开发板 SCH 定值**（U2 CH342F 区，Q2 S8050 单管方案）：
> RTS0→Q2基极(直连)、DTR0→R22(4.7k)→Q2基极、Q2集电极→R14(100Ω)→T5_RST、发射极→GND。
> T5_RST 上拉 R47(10k)+去耦 C51(0.1µF) 开发板已有（在 J2.2 引出），扩展板可不重复。
> ⚠️ CH340C 与开发板 CH342F 共用 UART0，**同时只插一个 Type-C**。

### 3× WS2812 + 电平转换
| Net | 从 | 到 | 备注 |
|-----|----|----|------|
| `WS2812_DIN_3V3` | H1(P13 / J1.16) | U_LS(SN74LV1T34) 输入A | 3.3V 侧 |
| `WS2812_DIN_5V` | U_LS 输出Y → R_LED 100-330Ω | LED1.DIN | 5V 侧 |
| chain | LED1.DOUT → LED2.DIN → LED3.DIN | | 菊花链 |
| `5V`/`GND` | H1 5V/GND | LED1-3 VDD/VSS、U_LS VCC | 每颗 C_LED 100nF 去耦 |

### 锂电池并联座 + 电池总开关
| Net | 从 | 到 | 备注 |
|-----|----|----|------|
| `BAT_CELL+` | J_BAT(+) | SW_BAT 一端 | 电池正极先进开关 |
| `BAT+` | SW_BAT 另一端 | 开发板 JST(CN1) BAT+ 同网络 | 开关后并入同一 BAT 网 |
| `GND`  | J_BAT(-) | GND | |

> ⚠️ 与开发板电池座并联同一 BAT 网，**同一时间只插一节电池**；充电由开发板 ETA6003 管。
> **SW_BAT = 电池总开关**（拨动开关 SK12D07VG5），串在扩展板电池 BAT+ 路径：
> 关断时切断本扩展板电池供电；**Type-C 插入时系统仍照常通电/充电**（开关只管电池路径）。

## 已确认（逐脚映射完成）
- ✅ J1/J2 每脚 ↔ Pxx：`T5AI-Core_V101-SCH.pdf` 第 5 页（见 pinmap.md §8）
- ✅ J2 FPC 每脚 ↔ LCD 信号：CL28CK230-18A 接线图（见 pinmap.md §3）
- ✅ SW6 6 脚 ↔ 方向/中心/公共：WS-1002 规格书电路图（①A ②Center ③B ④D ⑤Com ⑥C）

## 仍需实机确认
- R1/R2（SPI 阻尼 22Ω）是否需要：48MHz 走线短可省，打样时可先 0Ω 跳线
- 背光 LEDK1-4 的限流电阻阻值：按屏实际亮度调（接线图注 R1 可调）
- 方向 A/B/C/D ↔ 上/下/左/右 的最终对应：装配后按外壳朝向在固件标定

## 机器可读源
结构化数据见 `pinmap.json`；冲突校验见 `check_pins.py`（已通过，无冲突）。
