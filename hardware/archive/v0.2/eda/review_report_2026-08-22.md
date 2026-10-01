# 扩展底板原理图审核报告（双路验证）

> 对象：EasyEDA 工程 AgenticKeyboard / Board "Expansion Baseboard" / P1（版本 V1.0，2026-08-22）
> 方法：**双路交叉验证**
> - 网表路径：`hardware/eda/netlist_extract.js`（union-find, tol=6+结点合并）经 bridge 实时提取
>   → 44 器件 / 196 引脚 / 84 网络，存 `netlist_extracted.json`
> - 截屏路径：官方导出 `SCH_Expansion Baseboard_1-P1_2026-08-22.png/svg`，playwright 放大取证
>   （`zoom_reset_cap.png` `zoom_reset2.png` `zoom_q1.png` `zoom_q1_tight.png` `zoom_usb.png` `zoom_esd.png` `zoom_bl.png`）
> - datasheet：CH340C(SOP-16) / HE9073A33(SOT-23-5) / SN74LV1T34 / S8050 / WS-1002 / SK12D07VG5
> - **官方参考**：`T5AI-Core_V101-SCH.pdf`（PDF 文本+坐标提取 pdf_pos.mjs）/ `T5-MCU-Chip-Datasheet.pdf`
> 结论以官方 PNG 为权威裁决（几何提取在密集区 tol 敏感）。

> **【2026-08-23 复核更正】** 原报告把"复位管 Q1 发射极接 RTS# 未接 GND"列为严重错误。
> 经比对官方 `T5AI-Core_V101-SCH.pdf` 中 CH342F 的自动下载复位电路（Q2/S8050），
> **官方设计本身就是：基极←R22←DTR0、发射极→RTS0、集电极→R14→T5_RST，第三脚同样不接 GND**。
> 本板 Q1（基极←R2←DTR#、发射极→RTS#、集电极→R3→T5_RST）与官方拓扑**完全一致**。
> 故 **Q1 复位电路判定为正确，非错误**（撤销原严重项）。详见下方 §1 修正。

## 结论摘要（更正后）

| # | 事项 | 等级 | 证据 |
|---|------|------|------|
| ~~1~~ | ~~Q1 发射极接 RTS# 未接 GND~~ → **正确，照抄官方 T5AI-Core CH342F 复位电路** | ✅ 撤销 | 官方 SCH pdf_pos: DTR0→R22→base, RTS0→emitter, T5_RST→R14→collector；本板同拓扑 |
| 2 | 电池开关 SK12D07VG5 额定 0.3A，整机峰值可能超 | 中高 | datasheet 0.3A + 负载估算 |
| 3 | WS2812 DIN 缺串阻（U3.Y 直连 LED1.DIN） | 低 | 网表 WS_DIN_5V=[U3.4,LED1.3] |
| 4 | 背光 R6=10Ω 与文档 4.3R 记载不一致 | 低（确认值） | SVG 文本=10Ω |
| 5 | 电池并联"同时只插一节"需丝印强约束 | 低（工艺） | 设计约定 |

> 更正后**无阻塞性错误**；仅余 2-5 的建议/确认项。

---

## 1. 连线正确性 ✅（全部正确）

### 复位电路 Q1 —— 正确（照抄官方 T5AI-Core，2026-08-23 更正）
本板：Q1(S8050) 基极←R2(4.7k)←DTR#(pin13)；发射极→RTS#(pin14)；集电极→R3(100Ω)→T5_RST。
官方 `T5AI-Core_V101-SCH.pdf`（Q2/S8050，pdf 坐标提取证据）：
- 基极 PIQ201(761,110) ← R22 ← **NLDTR0**(698,112)
- 发射极 PIQ202(782,93) → **NLRTS0**(777,65)（**直连，同样不接 GND**）
- 集电极 PIQ203(782,121) → R14 → **NLT50RST**(777,193)
两者拓扑**一一对应**，Q1 设计正确。此为 CH34x 系列常见的 RTS/DTR 双信号一管复位方案
（EN/BOOT 由 DTR/RTS 组合时序驱动），发射极走 RTS 而非硬接地是**有意设计**，非缺陷。

### USB D+/D- 翻转 —— 正确（双路一致）
Type-C 4 根数据脚经**网络标签+结点**合并：
- `USB_DP1(A6) + USB_DP2(B6) + USB_DP(→CH340 D+) + D1(ESD)` 汇于一个 junction（zoom_esd.png 红点）
- `USB_DN1(A7) + USB_DN2(B7) + USB_DN(→CH340 D-) + D3(ESD)` 同理
→ 正反插两组 D+/D- 并联到 CH340，**翻转处理正确**。
（注：网表初次把 USB_DP1/DP2/DP 拆成 3 net，是 label 结点未 merge 的提取假象，视觉裁定为连通。）

### 其余网络两端齐全
KEY1-5 / NAV_×5 / LCD_×6 / UART0_TX/RX / CC1/CC2 / WS 链 / 电池 —— 均两端齐全，无短接/悬空。
真正的单节点网络只有 U4 备用 GPIO(P31/P3/P8/P7/P4/P34/P36/P44-47/P19/P18/P12/P2/DP/DN)、
CH340 NC 脚(7-12,15)、FPC 触摸脚(15-18)、U3.NC —— 均为合法预留/NC。

---

## 2. 原理正确性

### ✅ CH340C（USB-UART 桥）
- pin1 GND✓；pin2 TXD→**R1(2kΩ)**→UART0_RX✓；pin3 RXD→UART0_TX✓
- pin4 V3 与 pin16 VCC **都接 CH340_3V3**（V3↔VCC 短接正确，3.3V 供电必须接法）✓，去耦 C1(100nF)+C2(1uF)✓
- pin5 D+→USB_DP、pin6 D-→USB_DN，经 ESD 到 Type-C✓
- pin9/10/11/12/15 = NC(打叉)✓

### ✅ LDO（U2 HE9073A33, SOT-23-5）
pin1 VIN→VBUS，pin2 GND，**pin3 CE→VBUS（常使能，正确）**，pin4 NC(悬空)，pin5 VOUT→CH340_3V3。
输入 C12(10uF)+C5/C6，输出 C9(10uF)。VBUS 仅供 LDO，不灌系统✓。
（此前对 EN 的疑虑：CE 实为接 VIN 常使能，正确，解除。）

### ✅ 复位电路 Q1（S8050）—— 正确（照抄官方，2026-08-23 更正）
实际接法（zoom_q1_tight.png + netlist + 官方 SCH 三证）：
- 基极 B ← R2(4.7kΩ) ← CH340 DTR#(pin13)
- 发射极 E → CH340 RTS#(pin14)
- 集电极 C → R3(100Ω) → T5_RST

**与官方 `T5AI-Core_V101-SCH.pdf` 的 CH342F 复位管 Q2 完全一致**（DTR0→R22→base、RTS0→emitter、
T5_RST→R14→collector，第三脚同样不接 GND）。这是 CH34x USB-UART 桥常见的
**DTR/RTS 双信号一管自动下载复位**方案：DTR/RTS 的电平/时序组合经此管驱动 T5 的 RST（及 BOOT 时序），
发射极走 RTS 而非硬接地属**有意设计**。原报告误按"通用低边复位管发射极必接地"判为错误，**予以更正**。

> 说明：Q2 背光管（发射极→GND）与 Q1 复位管（发射极→RTS#）用途不同，不能互为样板——
> Q2 是纯低边开关，Q1 是 CH34x 复位专用拓扑。

### ✅ 背光低边开关 Q2（S8050）
LCD_BL(P5)→R7(4.7k)→基极；集电极→R6(10Ω)→LCD_LEDK(FPC 11-14 共阴)；发射极→GND；LEDA(FPC10)→VDD_3V3。低边调光，正确。

### ✅ 电平转换 + WS2812
U4.P13→WS_DIN_3V3→U3(SN74LV1T34).A；U3.Y→WS_DIN_5V→LED1.DIN；
LED1.DOUT→WS_D12→LED2；LED2.DOUT→WS_D23→LED3（菊花链✓）。
U3 VCC=VDD_5V（决定输出 5V 电平，方向 A→Y 正确✓）；每颗 LED VDD_5V/GND + 100nF(C7/C8/C10)✓。

### ✅ 电池
BT1(+)→BAT_CELL_P→SW1(SK12D07VG5)→BAT_P（并入 BAT 网 + TP_BAT 测试点 + C11 10uF）；BT1 其余脚→GND。开关串 BAT+ 路径✓。

---

## 3. 阻值/容值合理性（官方 SVG 文本读出）

| 位号 | 值 | 用途 | 评价 |
|------|----|------|------|
| R1 | 2kΩ | CH340 TXD→T5 RX 隔离 | ✅ 合理（与 CH342F 共总线，460800 RC 影响可忽略） |
| R2 | 4.7kΩ | DTR#→Q1 基极 | ✅ |
| R3 | 100Ω | Q1 集电极→T5_RST | ✅ |
| R4/R5 | 5.1kΩ | Type-C CC1/CC2 下拉 | ✅ UFP/设备角色正确 |
| R6 | **10Ω** | 背光 LEDK 阴极限流 | ⚠️ 文档曾写 4.3R，实图为 10Ω。共阴 4 白光 LED，VLED≈3.1V，(3.3−3.1)/10≈20mA 偏保守；**需实测亮度定值** |
| R7 | 4.7kΩ | 背光 Q2 基极 | ✅ |
| C1/C3/C7/C8/C10 | 100nF | 去耦 | ✅ |
| C2/C4 | 1uF | 去耦/储能 | ✅ |
| C5/C6/C9/C11/C12 | 10uF | LDO 输入输出 / 电池 | ✅ |

⚠️ **WS2812 DIN 无串阻**：U3.Y 直连 LED1.DIN（netlist WS_DIN_5V=[U3.4,LED1.3]）。建议补 100–330Ω 抑制首颗振铃（非阻塞）。

---

## 4. 按键消抖 ✅（设计选择合理）
- 5 机械键(K1-5) + 摇杆(JOY1) 均**低有效**：一端接 GPIO、一端接 GND，靠 **T5 内部上拉 + 固件消抖**。
- 无硬件 RC 消抖、无每脚 ESD（文档标注为可选）。对桌面小设备可接受。
- 摇杆 WS-1002 脚序 ①A ②Center ③B ④D ⑤Com ⑥C 与 datasheet 电路图一致，⑤Com→GND✓；pin7/8→GND。
- 建议（非阻塞）：若外壳金属化/走线长，至少给摇杆加 ESD 或每脚 100nF。

---

## 5. 其他问题
- ⚠️ **电池开关额定**：SK12D07VG5 = **DC50V/0.3A**（datasheet）。整机峰值 T5(~150mA)+LCD(~40mA)+WS2812(3 颗全白~180mA) **可能超 0.3A** → 固件限 LED 亮度到 <60mA，或换 0.5A/1A 开关。
- ⚠️ **电池并联安全**：扩展板电池座与开发板座并联同一 BAT 网，**同一时间只准插一节**（两节并联互灌危险）→ 丝印/说明书强约束。
- ✅ **VBUS 保险**：F1 = BSMD0805-050-6V（0.5A 保持自恢复）串在 VBUS，仅 CH340 支路，合理。
- ✅ **USB ESD**：D1/D3 = JEB05C TVS 阵列，D+/D- 对 GND，位置正确。
- ✅ **防倒灌**：CH340_3V3 独立轨（仅 LDO 供），VBUS 不接系统 5V，杜绝与开发板 CH342F/系统电倒灌。

---

## 必改项（阻塞）
**无。** （原列的 Q1 复位管发射极项，经比对官方 T5AI-Core 参考设计后**撤销** —— 本板照抄官方拓扑，正确。）

## 建议项（非阻塞）
1. 电池开关按峰值电流校核（限亮度或换更高额定）。
2. WS2812 DIN 补 100–330Ω 串阻。
3. 确认 R6 背光限流值（当前 10Ω，实测亮度）。
4. 电池"单节"丝印；摇杆可选 ESD。

## 正确、无需改动
Q1 复位管（照抄官方 CH342F 方案，发射极→RTS#）、
V3↔VCC 短接、CH340 独立 3V3 轨、TX 2k 隔离、VBUS 只供 LDO、CC 双 5.1k、
USB D+/D- 翻转合并、ESD 到位、电平转换方向/VCC=5V、WS 菊花链、
Q2 背光低边开关、LDO CE 常使能、电池开关串 BAT+。
