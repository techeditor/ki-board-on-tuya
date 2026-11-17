# v0.3 功能级网表 — Expansion Board_v0.3_lite

> 版本：v0.3.0 ｜ 核实日期：2026-10-01
>
> 供原理图复核、PCB导入和固件联调使用。实际位号对齐已审核的EasyEDA工程。
>
> 机器可读引脚源：[pinmap-v0.3.json](pinmap-v0.3.json)
>
> **适用范围**：本网表只适用于 **v0.3.0（lite）**。
> - v0.3.1 low-profile 的位号与本表不同，见 [netlist-v0.3.1-low-profile.md](netlist-v0.3.1-low-profile.md)。**R5–R8 在两版上指向不同器件**：
>
>   | 位号 | v0.3.0（本表） | v0.3.1 low-profile |
>   |---|---|---|
>   | R5 | 10Ω，LCD 背光 LEDK 限流 | 10kΩ，LCD TE 串阻 |
>   | R6 | 2.2kΩ，Q1 基极限流 | 10Ω，背光限流 |
>   | R7 | 10kΩ，Q1 基极下拉 | 2.2kΩ，Q1 基极 |
>   | R8 | 10kΩ，LCD TE 串阻 | 10kΩ，Q1 基极下拉 |
>
>   `check_pins_v0.3.py` 和 `bom-v0.3.md` 按 v0.3.0 的位号写，不能直接用于 v0.3.1。
> - v0.3.2_standard 的灯链电源开关（P18、Q2、Q3、R9–R12、C15）不在本表，也不在 v0.3.1 中，见 [pinmap-v0.3.md](pinmap-v0.3.md) §5。

## 1. 元件与位号

### 主接口、LCD和背光

| 位号 | 器件 | 立创编号/封装 | 用途 |
|---|---|---|---|
| U1 | TUYA_T5_CORE | 项目自建44P封装 | T5AI-Core双1×22排母接口 |
| FPC1 | FPC-05F-12PH20 | C2856799 / 12P 0.5mm下接 | T280C7-C12-26 LCD |
| R3/R4 | 0Ω | C106231 / 0402 | LCD MOSI/SCK串阻占位；需要时换22Ω |
| C13 | 1µF | C52923 / 0402 | LCD VDD储能 |
| C14 | 100nF | C343047 / 0402 | LCD VDD高频去耦 |
| Q1 | S8050 | C908252 / SOT-23 | 背光低边开关 |
| R5 | 10Ω | C2906983 / 0603 | LCD LEDK限流 |
| R6 | 2.2kΩ | C4190 / 0603 | Q1基极限流 |
| R7 | 10kΩ | C99198 / 0603 | Q1基极下拉 |
| R8 | 10kΩ | C99198 / 0603 | LCD TE到T5 P7保护串阻 |

### 六键与EC11

| 位号 | 器件 | 立创编号/封装 | 用途 |
|---|---|---|---|
| KEY1–KEY6 | Kailh轴座CPG151101S11 | C2803348 | 六个热插拔轴座，进入BOM |
| K1–K6 | MX轴体机械模板 | C49234237参考封装 | 机械孔/轮廓，不进BOM |
| U3 | EC1106S | C9900001788 | EC11增量编码器+按键 |
| R13/R15 | 10kΩ | C99198 / 0603 | A/B相外部上拉到3.3V |
| R14/R16 | 10kΩ | C99198 / 0603 | A/B相串联滤波 |
| C10/C11 | 10nF | C100042 / 0603 | A/B相对地滤波 |
| C12 | 100nF | C14663 / 0603 | EC11按键去抖 |

### WS2812

| 位号 | 器件 | 立创编号/封装 | 用途 |
|---|---|---|---|
| U2 | SN74LV1T34DBVR | C100024 / SOT-23-5 | P4的3.3V数据转换到5V |
| R1 | 100Ω | C22775 / 0603 | LED1数据入口串阻 |
| R2 | 0Ω | C106231 / 0402 | 3.3V直驱旁路；**DNP**，不得与U2同装 |
| C1 | 47µF/10V/X5R | C6119971 / 1206 | 5V灯链入口储能 |
| C2 | 100nF | C343047 / 0402 | U2去耦 |
| LED1–LED7 | XL-3528RGBW-2812B | C2890364 | 七颗可寻址RGB LED |
| C3–C9 | 100nF | C343047 / 0402 | 七颗LED逐颗去耦 |

### 机械件

| 位号 | 说明 |
|---|---|
| SCREW1–SCREW4 | 四个安装孔，不进SMT BOM |

## 2. U1 / T5AI-Core网络

| 网络 | U1/T5引脚 | 下游 |
|---|---|---|
| `VDD_3V3` | U1.3V3A/3V3B | LCD、EC11上拉和逻辑电路 |
| `VDD_5V` | U1.5V | U2、LED1–LED7、C1 |
| `GND` | U1.GNDA/GNDB/GNDC/GNDD | 全板地 |
| `WS_DIN_3V3` | P4 | U2.A；R2旁路一端 |
| `LCD_RST` | P6 | FPC1.2 |
| `LCD_TE` | P7 | R8→FPC1.7 |
| `LCD_BL` | P8 | R6→Q1.B |
| `LCD_SCK` | P14 | R4→FPC1.4 |
| `LCD_CS` | P15 | FPC1.5 |
| `LCD_MOSI` | P16 | R3→FPC1.3 |
| `LCD_DC` | P17 | FPC1.6 |
| `EC11_A` | P20 | R14输出、C10 |
| `EC11_B` | P21 | R16输出、C11 |
| `EC11_SW` | P22 | U3.D、C12 |
| `KEY5` | P23 | KEY5/K5 |
| `KEY1` | P24 | KEY1/K1 |
| `KEY2` | P25 | KEY2/K2 |
| `KEY3` | P26 | KEY3/K3 |
| `KEY4` | P27 | KEY4/K4 |
| `KEY6` | P32 | KEY6/K6 |

P2/P3/P5为SPI1固定组保留；P30、P31、P12/P13、P18/P19、P34/P36、P44–P47为空闲。UART0、RST、DP/DN不接扩展电路。

## 3. LCD网络

```text
P16/LCD_MOSI ─ R3 0Ω ─ FPC1.3/SDA
P14/LCD_SCK  ─ R4 0Ω ─ FPC1.4/SCL
P15/LCD_CS   ───────── FPC1.5/CSX
P17/LCD_DC   ───────── FPC1.6/DCX
P6/LCD_RST   ───────── FPC1.2/RESX
FPC1.7/TE    ─ R8 10k ─ P7/LCD_TE
```

电源与背光：

```text
VDD_3V3 ─ FPC1.9/VDD
         ├ FPC1.10/LEDA+
         ├ C13 1µF ─ GND
         └ C14 100nF ─ GND

P8/LCD_BL ─ R6 2.2k ─ Q1.B
                         └ R7 10k ─ GND
FPC1.11/LEDK− ─ R5 10Ω ─ Q1.C
Q1.E ─ GND
FPC1.1/8/12/13/14 ─ GND
```

## 4. 六键网络

每个按键网络同时连接机械模板Kx和电气轴座KEYx；Kx不进BOM，KEYx进入BOM：

```text
P24 ─ KEY1.1/K1.1；KEY1.2/K1.2 ─ GND
P25 ─ KEY2.1/K2.1；KEY2.2/K2.2 ─ GND
P26 ─ KEY3.1/K3.1；KEY3.2/K3.2 ─ GND
P27 ─ KEY4.1/K4.1；KEY4.2/K4.2 ─ GND
P23 ─ KEY5.1/K5.1；KEY5.2/K5.2 ─ GND
P32 ─ KEY6.1/K6.1；KEY6.2/K6.2 ─ GND
```

固件使用内部上拉和软件去抖，按下为低电平。

## 5. EC11网络

```text
VDD_3V3 ─ R13 10k ─┐
                    ├─ U3.1/A ─ R14 10k ─ EC11_A/P20
                    │                       └ C10 10nF ─ GND
GND ─ U3.2/C

VDD_3V3 ─ R15 10k ─┐
                    ├─ U3.3/B ─ R16 10k ─ EC11_B/P21
                    │                       └ C11 10nF ─ GND
GND ─ U3.4/E
U3.5/D ─ EC11_SW/P22
          └ C12 100nF ─ GND
U3.6/7/EP ─ GND
```

## 6. WS2812网络

U2：Pin1=NC、Pin2=A、Pin3=GND、Pin4=Y、Pin5=VDD_5V。

```text
P4/WS_DIN_3V3 ─ U2.A
U2.Y/WS_DIN_5V ─ R1 100Ω ─ LED1.DI
LED1.DO ─ LED2.DI ─ LED3.DI ─ LED4.DI ─ LED5.DI ─ LED6.DI ─ LED7.DI
LED7.DO ─ NC
```

每颗C2890364：Pin1=GND、Pin2=DI、Pin3=VDD_5V、Pin4=DO。C3–C9分别跨接对应LED的VDD和GND。

```text
VDD_5V ─ C1 47µF ─ GND
VDD_5V ─ C2 100nF ─ GND   # U2就近
```

R2跨接`WS_DIN_3V3`与`WS_DIN_5V`，只用于不安装U2时的3.3V直驱实验；默认DNP。

## 7. PCB结果摘要

- 元件54、网络36、走线292、过孔147；
- GND过孔134、VDD_5V过孔7；
- Top/Bottom各一块GND铺铜；
- 严格DRC：0；
- 不存在第七按键、NAV、LCD_PWR、扩展板USB/UART等旧网络。

## 8. 实物核验门槛

- FPC1使用Top层`_REV`封装，必须以实物确认下接、Pin1和排线折弯方向；
- 天线投影下若存在扩展板铜/走线，Top/Bottom必须同时挖空；
- MX机械孔与C2803348轴座需确认正反面与固定偏移；
- R2必须保持DNP。
