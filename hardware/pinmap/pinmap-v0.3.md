# v0.3 硬件引脚映射与连接方案

> 版本：项目v0.3 / 硬件v0.3.0
>
> EDA：`Expansion Board_v0.3_lite`
>
> 机器可读源：[pinmap-v0.3.json](pinmap-v0.3.json)
>
> 结论依据：2026-10-01原理图/PCB只读审核。v0.2 旧基线见带 `-v0.2` 后缀的文件；原无后缀副本已删除。

## 1. 系统边界

扩展板是无源功能板，只从T5AI-Core的44P排针取得3.3V、5V、GND和GPIO。扩展板没有：

- Type-C或USB-UART；
- 电池座、充电器或电源开关；
- 独立稳压器。

烧录、串口通信、电池充电、电量采样、音频和系统电源均由T5AI-Core承担。

## 2. v0.3最终引脚分配

| 功能 | T5引脚 | 44P位置 | 模式/电平 | 连接 |
|---|---|---|---|---|
| WS2812数据 | P4 | J2.20 | SPI1_MOSI输出 | `WS_DIN_3V3`→U2→R1→LED1 |
| LCD复位 | P6 | J2.18 | GPIO输出，低有效 | FPC1.2 |
| LCD TE | P7 | J2.17 | GPIO输入，无上下拉 | FPC1.7经R8=10k |
| LCD背光 | P8 | J2.16 | GPIO输出，高有效 | R6→Q1低边开关 |
| LCD时钟 | P14 | J1.14 | SPI0_SCK | R4→FPC1.4 |
| LCD片选 | P15 | J1.15 | GPIO输出，低有效 | FPC1.5 |
| LCD数据 | P16 | J1.13 | SPI0_MOSI | R3→FPC1.3 |
| LCD D/C | P17 | J1.12 | GPIO输出 | FPC1.6 |
| EC11 A相 | P20 | J2.4 | GPIO输入，外部10k上拉 | R14滤波后节点 |
| EC11 B相 | P21 | J2.5 | GPIO输入，外部10k上拉 | R16滤波后节点 |
| EC11按键 | P22 | J2.6 | GPIO输入上拉，低有效 | U3.D，C12对地 |
| KEY5 | P23 | J2.7 | GPIO输入上拉，低有效 | KEY5轴座 |
| KEY1 | P24 | J2.13 | GPIO输入上拉，低有效 | KEY1轴座 |
| KEY2 | P25 | J2.8 | GPIO输入上拉，低有效 | KEY2轴座 |
| KEY3 | P26 | J2.9 | GPIO输入上拉，低有效 | KEY3轴座 |
| KEY4 | P27 | J2.12 | GPIO输入上拉，低有效 | KEY4轴座 |
| KEY6 | P32 | J2.14 | GPIO输入上拉，低有效 | KEY6轴座 |

### 输入数量

- 6个机械键：6 GPIO；
- EC11：A、B、按键共3 GPIO；
- 合计9个输入GPIO。

P30在取消K7后保持空闲。

## 3. SPI资源

### SPI0：LCD

```text
P15 = CS
P14 = SCK
P16 = MOSI
P17 = SPI0_MISO固定组引脚，但v0.3复用为LCD_DC
```

LCD是只写SPI，不使用MISO。

### SPI1：WS2812

```text
P3 = SPI1_CS，保留，无外部负载
P2 = SPI1_SCK，保留，无外部负载
P4 = SPI1_MOSI，驱动WS2812
P5 = SPI1_MISO，保留，无外部负载
```

TuyaOpen初始化SPI1时可能配置整个固定引脚组，因此P2/P3/P5不得分配给其他功能。

### QSPI1/默认4-bit SDIO

QSPI1/默认SDIO使用P2–P7整组。该组已被SPI1、LCD_RST和LCD_TE占用，v0.3不支持完整QSPI1或默认4-bit SDIO；这不是新增损失。

## 4. 板载专用/保留引脚

| 引脚 | 开发板内部用途 | v0.3策略 |
|---|---|---|
| P0/P1 | UART1调试通道 | 保留，不接扩展负载 |
| P9 | 板载用户LED | 不复用 |
| P10/P11 | UART0与板载CH342F | 扩展板不连接 |
| P28 | `ADC_BAT` / AUXADC4 | 电量检测专用，未引出44P |
| P29 | 板载用户按键 | 不复用 |
| P38 | `CHG_DET_N` | 充电状态专用，未引出44P |
| P39 | 扬声器使能 | 不复用 |

电池监控：BAT经3MΩ/1MΩ分压后进入P28/ADC4；ETA6003状态进入P38。扩展板无需增加电量检测电路。

## 5. 空闲引脚

以下44P引脚在v0.3扩展板上保持空闲：

```text
P12, P13, P18, P19, P30, P31, P34, P36, P44, P45, P46, P47
```

v0.3.2 用途：**P18（J1.11）= `LED_PWR_EN`**，WS2812 灯链与 U2 的电源开关（P18 → R10 → Q3 S8050 → Q2 AO3401A 栅极，
高电平 = 灯链上电，R11 下拉保证 P18 浮空时关断），见 `hardware/eda/ws2812-power-reduction-study.md`。
lite / v0.3.1 上 P18 仍是空脚，固件同一镜像在三版上都驱动 P18（宏 `AK_PIN_LED_PWR_EN`）。

用途提示：P12/P13可作UART0 RTS/CTS；P31/P30可作UART2；P44–P47可作SPI0备用组，但不能形成第二个独立SPI0控制器。

## 6. LCD 12P映射

LCD：T280C7-C12-26，ST7789P3，240×320，4-wire SPI，无触摸。

| FPC1脚 | 信号 | 连接 |
|---:|---|---|
| 1 | GND | GND |
| 2 | RESX | P6/LCD_RST |
| 3 | SDA | P16经R3=0Ω |
| 4 | SCL | P14经R4=0Ω |
| 5 | CSX | P15 |
| 6 | DCX | P17 |
| 7 | TE | R8=10k→P7 |
| 8 | GND | GND |
| 9 | VDD | VDD_3V3，C13/C14去耦 |
| 10 | LEDA+ | VDD_3V3 |
| 11 | LEDK− | R5→Q1集电极 |
| 12 | GND | GND |
| 13/14 | 连接器固定脚 | GND |

P7不再是LCD_PWR。固件必须先把P7配置为输入；需要TE同步时发送`TEON(0x35)`，否则发送`TEOFF(0x34)`。

## 7. 六键与EC11

六键均为低有效：GPIO内部上拉，按下接GND。KEY1–KEY6是C2803348轴座；K1–K6仅提供MX轴体机械孔/轮廓，不进BOM。

EC11：

```text
U3.1 A ─┬─ R13 10k → VDD_3V3
        └─ R14 10k → EC11_A/P20；C10 10nF→GND
U3.2 C ─── GND
U3.3 B ─┬─ R15 10k → VDD_3V3
        └─ R16 10k → EC11_B/P21；C11 10nF→GND
U3.4 E ─── GND
U3.5 D ─── EC11_SW/P22；C12 100nF→GND
U3.6/7 EP ─ GND
```

## 8. WS2812

七颗均为C2890364 / XL-3528RGBW-2812B：Pin1=GND、Pin2=DI、Pin3=VDD_5V、Pin4=DO。

```text
P4 → U2(SN74LV1T34) → R1 100Ω → LED1 → LED2 → ... → LED7
```

LED1–LED6对应K1–K6，LED7对应EC11。R2=0Ω是直驱旁路，默认DNP，绝不能与U2同时贴装。

## 9. PCB审核摘要

- 54个元件、36个网络、292条走线、147个过孔；
- Top/Bottom双层GND铺铜；
- 严格DRC：0条错误；
- 信号10mil，3.3V为15–20mil，5V为20mil；
- 六键中心距19.10mm×19.10mm。

待实物确认：天线净空、FPC Pin1/下接方向、MX轴体与轴座组合、键距与外壳适配。
