# 网表 — Expansion Board v0.3.1 Low-Profile

> 来源：EasyEDA 工程 `Ki-board 矮轴版` / PCB `PCB_ExpBoard_v0.3.1_low-profile` 的**焊盘网络**（只读提取，2026-11-05），
> 并与原理图 `Expansion Board_v0.3.1_low-profile`（Main / Keys）重建的连通性逐脚对比：**一致**
> （K1–K6 轴体符号的 2 个引脚在封装上没有焊盘，属预期）。
>
> 位号以 EDA 为准。`$1Nxxx` / `$3Nxxx` 是 EDA 自动命名的网络，本表补了功能说明。

## 1. 元件清单

| 位号 | 器件 | 封装 | 面 | 说明 |
|---|---|---|---|---|
| U1 | TUYA_T5_CORE（2× 1×22 母排合为 44 脚封装） | TUYA_T5_CORE | Top | 插 T5AI-Core 开发板 J1/J2 |
| U2 | SN74LV1T34DBVR（C100024） | SOT-23-5 | Top | WS2812 数据 3.3V→5V 电平转换 |
| U3 | EC1106S（C9900001788） | SW-SMD_EC1106S | **Bottom** | EC11 旋转编码器 + 按键 |
| U4–U10 | WS2812B-MINI-V3J（C22371519） | LED-SMD_4P-L3.5-W3.5-P1.80-LS6.2_WS2812B-MINI-V3J | Top（反贴） | 7 颗 RGB 灯菊花链 |
| K1–K6 | 矮轴轴体（属性沿用 CPG151101D06 / C49234237，属 MX 料号，待改） | 九紫矮轴-白雪轴-无焊盘 | **Bottom** | 机械孔占位，不进 BOM |
| KEY1–KEY6 | 热插拔座（属性 Kailh CPG151101S11，属 MX 料号，待改） | CONN-SMD_MXT02 | Top | 每键一个座 |
| FPC1 | FPC-05F-12PH20（C2856799） | FPC-SMD_12P-P0.50_FPC-05F-12PH20_REV | **Bottom** | 12P 0.5mm 下接 LCD 座 |
| Q1 | S8050（C908252） | SOT-23-3 | Top | 背光低边开关 |
| R1 | 100Ω（C22775） | 0603 | Top | WS 链首串阻 |
| R2 | 0Ω（C106231），**DNP** | 0402 | Top | 电平转换旁路 |
| R3 / R4 | 0Ω（C106231） | 0402 | Top | LCD MOSI / SCK 串阻位 |
| R5 | 10k（C99198） | 0603 | Top | TE 串阻 |
| R6 | 10Ω（C2906983） | 0603 | Top | 背光限流 |
| R7 | 2.2k（C4190） | 0603 | Top | Q1 基极电阻 |
| R8 | 10k（C99198） | 0603 | Top | Q1 基极下拉 |
| R13 / R15 | 10k（C99198） | 0603 | Top | EC11 A / B 上拉 |
| R14 / R16 | 10k（C99198） | 0603 | Top | EC11 A / B 串联 |
| C1 | 47µF 10V X5R（C6119971） | 1206 | Top | 灯链 5V 储能 |
| C2 | 100nF（C14663） | 0603 | Top | U2 去耦 |
| C3–C9 | 100nF（C14663） | 0603 | Top | U4–U10 去耦（C3–C8 → U4–U9，C9 → U10） |
| C10 / C11 | 10nF（C100042） | 0603 | Top | EC11 A / B RC |
| C12 | 100nF（C14663） | 0603 | Top | EC11 SW 滤波 |
| C13 | 1µF（C1592） | 0603 | Top | LCD VDD 去耦 |
| C14 | 100nF（C14663） | 0603 | Top | LCD VDD 去耦 |
| SCREW1–4 | 安装孔 Ø2.70mm | M2螺丝 | — | 无网络 |

> 位号空缺：R9–R12（v0.3 删减后未重排），无影响。

## 2. 命名网络

| 网络 | 成员 |
|---|---|
| VDD_5V | U1.5V、U2.5、U4–U10.1、C1.2、C2.1、C3–C9.2 |
| VDD_3V3 | U1.3V3A、U1.3V3B、FPC1.9、FPC1.10、R13.1、R15.1、C13.2、C14.2 |
| GND | U1.GNDA–GNDD、U2.3、U3.2/4/6/7、U4–U10.3、KEY1–KEY6.2、FPC1.1/8/12/13/14、Q1.2、R8.2、C1.1、C2.2、C3–C9.1、C10.2、C11.1、C12.1、C13.1、C14.1 |
| KEY1 | U1.24（P24）、KEY1.1 |
| KEY2 | U1.25（P25）、KEY2.1 |
| KEY3 | U1.26（P26）、KEY3.1 |
| KEY4 | U1.27（P27）、KEY4.1 |
| KEY5 | U1.23（P23）、KEY5.1 |
| KEY6 | U1.32（P32）、KEY6.1 |
| EC11_A | U1.20（P20）、R14.1、C10.1 |
| EC11_B | U1.21（P21）、R16.2、C11.2 |
| EC11_SW | U1.22（P22）、U3.5（D）、C12.2 |
| WS_DIN_3V3 | U1.4（P4）、U2.2（A）、R2.1 |
| WS_DIN_5V | U2.4（Y）、R1.1、R2.2 |
| WS2812_END | U10.2（链尾 DOUT，单脚） |
| LCD_RST | U1.6（P6）、FPC1.2 |
| LCD_TE | U1.7（P7）、R5.1 |
| LCD_BL | U1.8（P8）、R7.1 |
| LCD_SCK | U1.14（P14）、R4.1 |
| LCD_MOSI | U1.16（P16）、R3.1 |
| LCD_CS | U1.15（P15）、FPC1.5 |
| LCD_DC | U1.17（P17）、FPC1.6 |

## 3. 自动命名网络（功能说明）

| EDA 网络 | 成员 | 功能 |
|---|---|---|
| $1N486 | R1.2、U4.4 | WS 链 → 第 1 颗 DIN |
| $1N320 | U4.2、U5.4 | U4 DOUT → U5 DIN |
| $1N361 | U5.2、U6.4 | U5 → U6 |
| $1N537 | U6.2、U7.4 | U6 → U7 |
| $1N550 | U7.2、U8.4 | U7 → U8 |
| $1N553 | U8.2、U9.4 | U8 → U9 |
| $1N551 | U9.2、U10.4 | U9 → U10 |
| $1N513 | R3.2、FPC1.3 | LCD SDA |
| $1N511 | R4.2、FPC1.4 | LCD SCL |
| $1N558 | R5.2、FPC1.7 | LCD TE |
| $1N554 | R6.2、FPC1.11 | LCD LEDK |
| $1N468 | R6.1、Q1.3 | Q1 集电极 |
| $1N469 | Q1.1、R7.2、R8.1 | Q1 基极 |
| $3N84 | U3.1（A）、R13.2、R14.2 | EC11 A 相端子 |
| $3N100 | U3.3（B）、R15.2、R16.1 | EC11 B 相端子 |

## 4. 无连接

- U1：P2、P3、P5、P12、P13、P18、P19、P30、P31、P34、P36、P44–P47、TX、RX、RST、DP、DN。
- U2.1（NC）。
- K1–K6 的 2 个符号引脚（封装无焊盘）。
- SCREW1–4（无网络）。
