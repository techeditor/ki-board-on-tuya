# EasyEDA 绘图会话记录（扩展底板原理图）

> 通过 easyeda-api skill + bridge(端口 49620) 驱动 EasyEDA Pro 客户端绘制扩展底板原理图。
> 执行代码用 `node exec.mjs -c "<js>"`（在本目录运行）。

## 工程 / 文档 UUID

| 项 | 值 |
|----|----|
| Project | AgenticKeyboard（uuid `410e857de6054445a2098dc8fe7c70df`） |
| Board | **Expansion Baseboard** |
| Schematic UUID | `e17691a21074b3c7` |
| **原理图页 UUID（作图目标）** | `41791b2bf2de7050` |
| PCB | PCB4 |
| EDA windowId | `02e28543-6e74-4d9d-a362-0e82b006ac5c` |

> 其它 Board（勿动）：`Joe方案`（schematic1/PCB1）、`Tuya T5AI Core`（v1.0/PCB3）。

## 作图约定

- **原理图坐标单位 = 0.01 inch (10 mil)**；1mm ≈ 3.937 单位。放置坐标要用该单位。
- 放器件：`eda.sch_PrimitiveComponent.create({libraryUuid, uuid}, x, y, subPart, rotation, mirror, addToBom, addToPcb)`。
- 器件优先按立创编号从元件库搜（`eda.lib_Device.search("Cxxxx" 或 型号)`）。
- 所有 API 调用 await；枚举用 enum 成员，不用裸数字。

## 操作原则 / 经验（重要，2026-08-22 总结）

1. ⚠️ **移动器件会导致已画导线脱落**：EasyEDA 里手动画的导线**不随器件移动**。
   每次拖动/翻转器件后，其引脚的连线会悬空或错连，必须按新坐标重连。
   → 器件位置定稿后再连线；若已连线后又移动，需重新核查该器件所有脚。
2. **核查连接必须用"当前实时坐标 + 电源地符号解析"**：
   - 先 `getAllPinsByPrimitiveId` 取**当前**引脚坐标（不能用记忆中的旧坐标）；
   - 引脚 net 判定：导线自带 net 用其 net；导线 net 为空(`""`)时，追踪导线端点是否落在
     Power/Ground 符号(NetFlag)附近(容差~15)来确定网络。只看导线 net 字段会漏判电源/地。
3. **删除区域连线要留边界余量**：按坐标框删线时，相邻脚(间距10)易被误删；删后要复查边界脚(如 pin8)。
4. **器件按立创编号取封装**；`getByLcscIds` 批量取 device uuid。
5. **规格书优先于既有文档**：CL28CK230 的 18P 脚序曾在 pinmap 里记错(1=RST)，
   以实物规格书 `CL28CK230-18A_SPEC_V0` PAGE7 + 接线图为准(1=GND)。核对硬件先读 datasheet。

## 绘制分块计划（对应 hardware/pinmap/netlist.md）

1. 排针接口 H1/H2（44P = 2×1×22），逐脚标网络名
2. 输入区：5 键(SW1-5) + 摇杆(SW6) → J2 各脚
3. LCD：18P FPC 座(J_LCD) + SPI/电源/背光
4. Type-C(J_USBC) + CH340C(U_USB) + LDO(U_LDO) + S8050(Q_RST) 自动复位
5. 电池座(J_BAT) + 电池总开关(SW_BAT)
6. 3×WS2812(LED1-3) + SN74LV1T34(U_LS) 电平转换
7. 阻容/ESD 补齐 → DRC

## 绘制完成状态（2026-08-21）

**原理图 "Expansion Baseboard" 已绘制完成并连线。**

### 后续核查与修复（2026-08-22，最新，覆盖以下过时描述）
> 以下修复在"布局美化/U4接入"之后进行，是当前最新状态。若与下方旧段落冲突，以本段为准。

- **LCD（FPC1）按规格书重连 + 加背光限流电阻**（依据 `datasheets/CL28CK230-18A_RTP&WithoutTP_230801/`
  的 `CL28CK230-18A_SPEC_V0` PAGE7 引脚表、PAGE4 背光特性、接线图）：
  - **脚序整体错位纠正**：此前误按 1=RST，实际规格书 **1=GND**（整体差一位）。已全部重连：
    1=GND, 2=RST, 3=SCL, 4=RS/DC, 5=CS, 6=SDA, 7=SDO(空), 8=GND, 9=VCC(3.3V),
    10=LEDA, 11-14=LEDK1-4(→GND), 15-18=触摸(空), 19/20=安装脚(→GND)。
  - **VCC 从 5V 改 3.3V**（规格书屏电源 2.8–3.3V，原接 5V 有超压风险）。
  - **背光限流电阻 R6 = 4.3Ω（对齐接线图 4R33；0603，立创 C22859 器件，Value 标 4.3R）**：
    回路 `3.3V → LEDA(10) →〔屏内 4 白光 LED 并联〕→ LEDK1-4(11-14)=K → R6 → GND`。
    ⚠️ **限流电阻在阴极侧（K→GND）**，不是 LEDA 侧；LEDA 阳极直接接 3.3V。VLED 2.9–3.3V/80–100mA，R6 可按亮度调。
  - `hardware/pinmap/pinmap.md` §3 的 18P FPC 表已同步更正（含背光阴极侧限流拓扑）。
- **JOY1 换真实符号**：由 1×6 排针占位改为 **WS-1002-ACW05026（8 脚）**。5 方向重连：
  pin1→NAV_UP, pin2→NAV_CENTER, pin3→NAV_LEFT, pin4→NAV_RIGHT, pin6→NAV_DOWN；pin5/7/8→GND(公共)。
  两端（JOY 侧 + U4 P20/21/22/30/32）均连通。
- **WS2812 链路修复**：曾因器件移动导致数据线断/U3输入输出短接，已修复为
  U4.P13→U3.A→(电平转换)→U3.Y→LED1.DIN→LED1.DOUT→LED2→LED3；LED VDD→5V符号、VSS→GND符号。
- **U3(SN74LV1T34) 移动后重连**：A→WS_DIN_3V3, Y→WS_DIN_5V, VCC→VDD_5V, GND→GND（方向/电平正确）。
- **电容容值补齐**：C1/C3/C7/C8/C9=100nF(CL05B104)，C2/C4/C5/C6=1uF(CL05A105)，已写入 Value 显示。
- **R6 位号说明**：R6 现为 **LCD 背光限流电阻(10Ω)**；下方旧段落里"R6(200R) WS2812 串阻"的说法已作废
  （WS2812 当前 U3.Y 直连 LED1.DIN，未用串阻）。
- ⚠️ 提醒：EasyEDA 里手动导线不随器件移动，若再拖动器件需重连对应网络。

### U4 接入 + 全导线网络（2026-08-22 二次调整）
- 用户手工新增 **U4 = TUYA_T5_CORE**（44 脚，对应开发板 J1+J2 排针，引脚名=P编号/TX/RX/RST/DP/DN/5V/3V3/GND）
- **删除占位排针 H1/H2**，改用 U4 作为唯一开发板接口
- **按指示不再使用双向端口(NetPort)符号**：33 个 BI NetPort 全部删除，网络改由**带 net 名的导线**表示
- U4 逐脚连线：信号脚拉带语义网名导线（P14→LCD_SCK, P24→KEY1, P32→NAV_UP, TX→UART0_TX 等按 pinmap 映射）；
  电源脚(5V/3V3A/3V3B)→Power 符号；地脚(GNDA-D)→Ground 符号
- 验证：remainingNetports=0，60 个电源地符号，36 网络，119 导线；
  所有信号网络两端齐全（器件侧导线 + U4侧导线，同名 net 连通）

### 布局美化重构（2026-08-22）
按用户要求重构：
- **电源/地用专门符号**：GND→Ground NetFlag；VDD_3V3/VDD_5V/VBUS/CH340_3V3→Power NetFlag（共 60 个电源地符号）
- **相邻器件引脚直连**（纯导线，无端口）：CH340_TXD(U1.2→R1.2)、CH340_DTR(U1.13→R2.1)、
  Q_BASE(R2.2/U1.14→Q1.B)、Q_COL(Q1.C→R3.1)、WS 菊花链(U3.Y→LED1→LED2→LED3)、BAT_CELL_P(BT1→SW1)
- **其它信号用导线+网名标签**（NetPort，同名连通）：LCD/KEY/NAV/UART/CC/USB/WS_DIN_3V3/T5_RST/LCD_PWR
- **紧凑重排**：接口列(H1/H2 最左)→LCD(贴H1)→USB/CH340/复位区(中)→输入(贴H2下)→WS2812一行(右)→电池(下)
- 结果：36 网络、114 导线、60 电源地符号。相邻直连网络(CH340_TXD/DTR/Q_BASE/Q_COL/BAT_CELL_P)
  为一根多段导线连两引脚；BAT_P 单节点=对外飞线(正确)。

### 首次绘制（历史）

器件（31 个，全部真实立创元件，位号已规范化）：
U1 CH340C / U2 HE9073A33(LDO) / U3 SN74LV1T34 / Q1 SS8050 / FPC1 LCD-18P /
USBC1 Type-C / BT1 电池座 / SW1 SK12D07VG5 / LED1-3 WS2812 / K1-5 机械键 /
JOY1 摇杆(1x6占位) / H1 H2 排针接口(1x20×2) / R1(2k) R2(4.7k) R3(100R) R4/R5(5.1k) /
C1-C9 去耦(100nF/1uF)。

网络验证（sch_PrimitiveWire.getAll 全量统计）：
- 39 个网络，124 根导线，187 个组件图元（含 NetPort）
- 38 个网络 ≥2 节点闭合；唯一单节点 = BAT_P（对外飞线到开发板 JST，设计正确）
- 关键网络节点数：GND=30, VBUS=7, VDD_3V3=5, VDD_5V=5, CH340_3V3=5, LCD_BL=5,
  USB_DP/DN=3, 各 LCD/KEY/NAV/CC/UART 信号=2, 菊花链 WS_DIN_5V/WS_D12/WS_D23=2, LCD_PWR=2

连线方式：每引脚端点画带 net 名短导线 + 同名 NetPort，同名网络自动连通（EasyEDA 标准做法）。

待人工微调（非阻塞）：
- JOY1 用 1x6 排针占位，PCB 阶段换 WS-1002 实际封装（脚序①A②C③B④D⑤Com⑥C 按 netlist）
- H1/H2 用 1x20 排针占位表示 44P 接口，PCB 阶段换 2×22 母排(行距 25.502mm)
- WS2812 串阻 R6(200R) 可选，当前 U3.Y 直连 LED1.DIN
- 器件位置较密，可在 EDA 里手动美化布局

## 器件（立创编号，来自 bom.md）

libraryUuid 统一 = `0819f05c4eef4c71ace90d822a990e87`（系统库）

| 位号 | 器件 | 立创编号 | device uuid |
|------|------|---------|-------------|
| U_USB | CH340C | C84681 | 4951b3094e3e48a7b4c8c708b64932e4 |
| U_LS | SN74LV1T34 | C100024 | 29f8986e12b04fa58c13b861456ed383 |
| Q_RST | S8050(SS8050) | C2150 | 02b834e6b4e94f2c8e7614e1e1c3ef53 |
| LED1-3 | WS2812B-B/T 5050 | C2761795 | c5278819af9446bfabff90179dc02240 |
| U_LDO | HE9073A33M5R | C723789 | 67695ce48f6747ffa41691d6e8bcb3ce |
| SW_BAT | SK12D07VG5 | C431548 | 029f1c261e3e492a93da9c8a9391621e |
| J_LCD | 18P 0.5mm FPC 下接 | C51901319 | 11c9d5d2cf15448a981e9ad609bdf39b |
| SW1-5 | CPG151101D06 茶轴 | C49234237 | 4b1424c162934d6cb69f4b98aaa1bc5b |
| J_USBC | Type-C 16P 卧贴 | C9900000202 | 1a971da2aebe4423aab6d767cc2d591b |
| J_BAT | JST/GH 1.25 2P 卧贴 | C9900014668 | bcc927f835514f8ebe7e0b464e692431 |
| SW6 | WS-1002 5向摇杆 | 库存件 | 待查/建符号 |
| H1/H2 | 1×22 2.54 排母 | 无现成 22P | 用 NetPort/NetLabel 表达接口 |

> 注：H1/H2（44P 接口）用网络标签(NetLabel)/网络端口(NetPort) 表达每脚网络名，
> 器件按同名网络自动连通；真实母排座封装在 PCB 阶段再定。
> 单位 0.01inch。create 已验证可用（CH340C 放置成功 id 81a6d21519dd72f0）。


## 启动 JLCEDA / easyeda-api bridge 的经验（2026-08-22，避免重复踩坑）

> 目标：让 `node exec.mjs -c "<js>"` 能连上 EDA。链路是三段：
> **AI(本 shell) ↔ bridge-server(Node, 端口 49620-49629) ↔ EasyEDA 客户端(装 run-api-gateway.eext 扩展)**。
> `exec.mjs` 连的是**中间的 bridge-server**，不是 EDA 客户端本身。

### 排障顺序（先判断断在哪一段）
1. `node exec.mjs -c "return 'ping'"`
   - 返回 `{"success":true,...,"windowId":"..."}` → 全链路 OK，直接干活。
   - `ECONNREFUSED 127.0.0.1:49620` → **bridge-server 没跑**（不是 EDA 的问题）。
2. 只开 EDA 客户端 **不够**：还必须
   - (a) bridge-server 在后台运行；
   - (b) EDA 里加载 `run-api-gateway.eext` 扩展（它会自动 WS 连到 bridge）。
   - 下载：https://jlc-ext.com/item/oshwhub/run-api-gateway

### ✅ 可靠的启动方法（本次实测成功）
用一个 **detached 子进程** launcher 拉起 bridge（见 `hardware/eda/launch_bridge.mjs`）：
```
node launch_bridge.mjs   # 在 hardware/eda/ 下运行
```
它用 `child_process.spawn(node, [bridge-server.mjs], {detached:true, stdio:[ignore,out,err]})`
+ `child.unref()`，spawn 完立即退出，bridge 留在后台。
skill 路径：`C:\Users\puzhang\.kiro\skills\easyeda-api-skill`（`node_modules` 已装，无需 npm install）。
bridge 脚本：`scripts\bridge-server.mjs`；端口自动在 49620-49629 里选第一个可用的。

### ❌ 本机 Windows 上**不奏效/踩坑**的方式（别再试）
- 通过本 `shell` 工具执行 **PowerShell `Start-Process ... -RedirectStandardOutput/Error`** 或
  **`cmd /c start "" /b node ...`** 后台起服务：多次返回 `exit code 1 + 空 stdout`，且
  `bstart.out/err` 根本没生成 —— 说明 node 压根没被拉起。多语句 PowerShell 经此 shell 传参
  经常被吞输出，不可信。
- 用 `Invoke-RestMethod`/`curl` 循环探 `/health`、`Get-NetTCPConnection` 探端口：
  在此 shell 里常返回空/exit 1，**不能据此判定 bridge 死活**。→ 改用 `exec.mjs` 探活最准。
- SKILL.md 里的 bash 片段（`for port in $(seq ...)`、`node ... &`）是 **bash 语法**，
  本机 Windows 默认 shell 跑不通，别照抄。

### 排障要点
- `bridge.log` 里若全是 `[WS] Ping ... pong` 然后 `EDA window disconnected (1001)`：
  说明 bridge 之前活着、但 **EDA 那一端的标签页被关/刷新** 断了；重开 EDA 页或重载扩展即可，
  bridge 不用重启。
- **windowId 会变**：每次重连 EDA 页，windowId 不同（本次为 `a89f0f6c-9a62-43e8-a425-e55e3cbd6e23`，
  上文表格里的旧值已过期）。多窗口时用 `GET /eda-windows` + `POST /eda-windows/select` 选。
- 起了多个 bridge 实例也没大碍（端口自增），但建议只留一个；清理就 kill 对应 node PID。
