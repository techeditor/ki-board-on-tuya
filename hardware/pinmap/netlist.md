# 网表（功能级）— 扩展底板

> 供 jlceda-mcp 或人工绘制原理图的连接清单。
> 44P 排针（开发板 J1/J2 两排）与 18P FPC、摇杆脚序**逐脚已确认**（见 pinmap.md §3/§4/§8）。
> **位号说明（2026-08-24 对齐 EDA v1.0）**：本表左列为**功能位号**（设计意图命名）；
> 「EDA 实际位号」列为 **EasyEDA 工程 Expansion Baseboard_v1.0** 中的真实位号（以 EDA 为准）。
> 二者可能不同：例如两个 1×22 母排在 EDA 中合并为单个 44 脚封装 **U1（TUYA_T5_CORE）**；摇杆在 EDA 中为 **SW2**。
> ⚠️ 电阻/电容**阻容实际值以 EDA 原理图为准**（本表数值为设计目标值，EDA 属性未在此逐一解析）。

## 元件清单（Designator）

| 功能位号 | EDA 实际位号(v1.0) | 元件 | 说明 |
|------|------|------|------|
| H1/H2（母排） | **U1**（封装 TUYA_T5_CORE，44脚=22×2） | 2× 1×22 2.54mm 母排（合并为一个 44 脚封装） | 插开发板 J1+J2 两排（3V3/5V/GND/LCD SPI/输入 GPIO/UART0/T5_RST 等） |
| J_LCD | **FPC1**（底层） | 18P 0.5mm FPC 座（国连 C51901319） | 接 CL28CK230-18A LCD。⚠️ **v1.0 缺陷：脚序镜像（Pin1↔Pin18 反）**，底层放置未做脚序镜像。下方逻辑映射正确；本版实物须**反向插入排线**才正常（2026-08-31 实机确认，见 TEST_RESULTS.md Step 4）。下一版改板修正 footprint。💡 **固件适配点**：此屏需 ST7789 **INVOFF**（默认序列结尾是 INVON，会导致全屏色彩取反）；正式固件 LCD 模块须沿用 no-inversion 初始化序列 |
| SW1..SW5 | **K1..K5** | 机械键 CPG151101D06 茶轴（直焊） | 5× 机械按键 |
| SW6（摇杆） | **SW2**（8脚）+ **U4**（2x4 扩展焊盘） | 5向摇杆 WS-1002 | 4向+中心；U4 为摇杆扩展/备用焊盘 |
| J_USBC | **USBC1** | Type-C 母座（16P） | 扩展板 USB 口（烧录+通信） |
| U_USB | **U3**（SOP-16） | CH340C | USB-转-UART 桥 |
| U_LDO | **U5**（SOT-23-5） | HE9073A33M5R（3.3V,库存） | Type-C VBUS 5V → 3.3V 给 CH340C |
| Q_RST | **Q2**（SOT-23） | S8050（NPN） | 自动复位三极管（RTS/DTR→基极，集电极→R_RST→T5_RST） |
| Q_BL | **Q1**（SOT-23） | S8050（NPN） | LCD 背光低边开关（LCD_BL→R_BL→基极，集电极→R_LEDK→LCD_LEDK） |
| U_LS | **U2**（SOT-23-5） | SN74LV1T34 | WS2812 电平转换 3.3→5V |
| LED1..LED3 | **LED1..LED3** | WS2812 RGB LED | 菊花链（3 颗） |
| **LED_VBUS** | **LED4** | 普通红色 0603 LED | **VBUS(Type-C 5V) 供电指示**（VBUS→R6→LED4→GND） |
| **LED_BAT** | **LED5** | 普通红色 0603 LED | **电池(BAT_P) 供电指示**（BAT_P→R10→LED5→GND） |
| J_BAT | **BT1** | JST PH 1.25mm 2P 座 | 锂电池并联座 |
| （2P 插针） | **H1** | HDR 2P 2.54mm 插针 | 电池相关 2P 插针（网络 GND / BAT_P） |
| SW_BAT | **SW1** | 拨动开关 SK12D07VG5（C431548，库存） | 电池总开关，串于 BAT+ |
| TP_VBUS | **TP1** | 测试点 | VBUS 测试点 |
| SCREW1..6 | **SCREW1..6** | M2.5 安装孔 | 外壳固定（非贴装） |
| R_SPI（阻尼） | **R11**(SCK)/**R12**(MOSI) | 22Ω 0402（可选，阻值以 EDA 为准） | LCD SPI SCK/MOSI 阻尼 |
| R_TX | **R5** | CH340C TX→T5 RX 隔离（阻值以 EDA 为准，设计目标 2kΩ） | UART0_RX 串阻（$3N37↔UART0_RX） |
| R_CC1/R_CC2 | （Type-C 区 R） | 5.1kΩ 0402 | Type-C CC 下拉（UFP/设备角色） |
| R_DTR | （复位区 R） | 4.7kΩ 0402（=开发板 R22） | CH340C DTR → Q_RST 基极 串阻 |
| R_RST | **R9** | 100Ω 0402（=开发板 R14，阻值以 EDA 为准） | Q_RST 集电极 → T5_RST 串阻 |
| R_WS_DIN | **R3** | WS2812 DIN 串阻（阻值以 EDA 为准，设计目标 100–330Ω） | U2.Y→R3→LED1.DIN（**EDA 已具备此串阻**） |
| R_BL / R_LEDK | **R2**(BL→基极)/**R1**(LEDK 限流) | 背光管基极电阻 / LEDK 限流（阻值以 EDA 为准） | 背光低边开关 Q1 外围 |
| R_LED_VBUS | **R6** | VBUS 指示灯限流（阻值以 EDA 为准） | 串 LED4 |
| R_LED_BAT | **R10** | BAT 指示灯限流（阻值以 EDA 为准） | 串 LED5 |
| （其他 R） | **R4 / R7 / R8** | 复位/去耦网络相关（用途见原理图） | R7 连复位($3N35/$3N36) |
| C_USB1/2 | （CH340 区 C） | 100nF + 1µF | CH340C VCC 去耦/储能 |
| C_LED1..3 | （LED 区 C） | 100nF ×3 | 每颗 WS2812 去耦 |
| C_FPC | （FPC 区 C） | 1µF + 100nF | LCD FPC 电源去耦 |
| D_ESD | **D1**(VBUS,SOD-123) / **D2·D3**(USB D+/D-,D0402-BI) | ESD/TVS | VBUS 与 USB D+/D- 静电防护 |

> 说明：C 类电容位号（C1..C13）在 EDA 中按布局编号，具体归属见原理图；本表按功能分组，阻容实际值以 EDA 原理图为准。

## 网络（Net）连接

### LCD SPI（SPI 信号 SCK/MOSI/CS/DC 走排针 J1；RST/BL/PWR 走 J2；右列为 18P FPC 屏排线脚号）
| Net | 从（T5引脚/排针） | 到（18P FPC 脚） | 备注 |
|-----|------------------|----------------|------|
| `LCD_SCK`  | P14 / J1.14 | FPC.3 (SCL)（经 R11 22Ω） | 48MHz |
| `LCD_MOSI` | P16 / J1.13 | FPC.6 (SDA)（经 R12 22Ω） | |
| `LCD_CS`   | P15 / J1.15 | FPC.5 (CS) | |
| `LCD_DC`   | P17 / J1.12 | FPC.4 (RS) | |
| `LCD_RST`  | P6  / J2.18 | FPC.2 (RST) | |
| `LCD_BL`   | P5  / J2.19 | FPC.10-14 (LEDA + LEDK1-4 经限流 R) | 高有效 |
| `LCD_PWR`  | P7  / J2.17 | FPC.9 (VCC) | 高有效，屏 3.3V 使能 |
| `GND`      | GND | FPC.1 / FPC.8 | |
| （预留触摸） | — | FPC.15-18 (XL/YU/XR/YD) | 一期不接 |

### 输入（低有效，公共端 GND；上拉用 T5 内部上拉）
| Net | 从（J2 / T5引脚） | 到 |
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
> ⚠️ **USB 隔离**：扩展板 Type-C 的 D+/D- **只接 CH340C 的 UD+/UD-**，是一条独立的 USB-转-UART 链路；
> **不连**排针 J2 上引出的 **DP/DN**（那是开发板 T5 的 **USB Host** 信号，属另一套 USB）。二者互不相连，
> 扩展板不使用/不引出排针 DP/DN。

### 3× WS2812 + 电平转换
| Net | 从 | 到 | 备注 |
|-----|----|----|------|
| `WS2812_DIN_3V3` | H1(P13 / J1.16) | U_LS(SN74LV1T34) 输入A | 3.3V 侧 |
| `WS2812_DIN_5V` | U_LS 输出Y → R_LED 100-330Ω | LED1.DIN | 5V 侧 |
| chain | LED1.DOUT → LED2.DIN → LED3.DIN | | 菊花链 |
| `5V`/`GND` | H1 5V/GND | LED1-3 VDD/VSS、U_LS VCC | 每颗 C_LED 100nF 去耦 |

### 2× 电源指示灯（普通红色 LED，EDA v1.0 新增，2026-08-24 补录）
| Net | 从 | 到 | 备注 |
|-----|----|----|------|
| `VBUS` | VBUS | R6(LED_VBUS 限流) → **LED4** 阳极；LED4 阴极 → GND | **Type-C VBUS(5V) 供电指示**（插 Type-C 亮） |
| `BAT_P` | BAT_P | R10(LED_BAT 限流) → **LED5** 阳极；LED5 阴极 → GND | **电池供电指示**（电池供电亮） |

> LED4/LED5 为普通红色 0603 LED（非 WS2812），仅作电源状态指示。限流电阻 R6/R10 阻值以 EDA 原理图为准。

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
- SPI 阻尼电阻（EDA 位号 **R11**=SCK / **R12**=MOSI）是否需要：48MHz 走线短可省，打样时可先 0Ω 跳线
- 背光限流电阻（EDA 位号 **R1**，LCD_LEDK 侧）阻值：按屏实际亮度调
- 方向 A/B/C/D ↔ 上/下/左/右 的最终对应：装配后按外壳朝向在固件标定
- ⚠️ 各电阻/电容**实际阻容值以 EDA 原理图（Expansion Baseboard_v1.0）为准**；本网表数值为设计目标值

## 机器可读源
结构化数据见 `pinmap.json`；冲突校验见 `check_pins.py`（已通过，无冲突）。
