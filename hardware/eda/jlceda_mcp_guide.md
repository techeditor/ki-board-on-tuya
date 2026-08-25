# JLCEDA MCP（EasyEDA Bridge）功能与使用汇总

> 用途：把嘉立创EDA（EasyEDA Pro）的操作能力通过 AI 自动化。本文汇总其架构、功能范围、
> 启动与连接方式、API 体系、关键坑与本项目实测用法。
> 来源：`easyeda-api` 技能 `SKILL.md`（version 1.1.3）+ 本项目 2026-08-24 实测（PCB 只读审核）。
> 环境：本机 **Windows**（PowerShell 调 HTTP），Node v22。
> 术语：**嘉立创EDA 的英文名 = EasyEDA**，同一产品；勿用 EASYEDA/easyeda 等其他写法。

---

## 1. 它是什么

一套让 AI Agent **控制正在运行的 EasyEDA Pro 客户端** 的桥接机制。严格说不是标准 MCP server，
而是一个 **Bridge Server（Node.js）** + **EasyEDA 扩展（run-api-gateway.eext）**，两端通过 WebSocket
互联；AI 侧通过 HTTP `POST /execute` 把 JS 代码送进 EDA 浏览器运行时执行。

能力面：
1. **完整 API 参考**：120 类 / 62 枚举 / 70 接口 / 19 类型别名。
2. **WebSocket 桥**：在运行中的 EasyEDA Pro 客户端里执行代码（实时调试）。
3. **代码模式**：PCB / 原理图 / 库 / 工程管理的常用操作范式。
4. **文档源格式参考**：直接分析/修改 EasyEDA 工程、原理图、PCB 源文件格式（不走 API 时）。
5. **扩展开发支持**：查 API、写扩展代码、桥接调试。

---

## 2. 架构

```
┌───────────┐   HTTP/WS     ┌─────────────────┐   WebSocket   ┌───────────┐
│ AI Agent  │ ◄───────────► │  Bridge Server  │ ◄───────────► │  EasyEDA  │
│ (本 CLI)  │  端口 49620-  │  (Node.js)      │  端口 49620-  │  (客户端) │
└───────────┘     49629     └─────────────────┘     49629     └───────────┘
```

- Bridge 启动时在 **49620–49629** 里自动选一个可用端口。
- AI 端与 EDA 端都通过扫描端口 + 校验握手（`service:"easyeda-bridge"`）自动发现服务。
- 支持 **多 EDA 窗口**：可列出/选择/指定窗口执行。

---

## 3. 启动与连接（Windows 实测）

### 3.1 技能目录
- 路径：`C:\Users\puzhang\.kiro\skills\easyeda-api-skill\`
- 关键文件：`scripts/bridge-server.mjs`（桥服务）、`references/`（API 文档）、`format/`（源格式）。
- 依赖已安装（`node_modules` 存在）；如缺失：`cd <skill dir> && npm install`。

### 3.2 启动 Bridge（必须后台运行，勿前台阻塞）

```powershell
$d="C:\Users\puzhang\.kiro\skills\easyeda-api-skill"
Start-Process -FilePath "node" -ArgumentList "`"$d\scripts\bridge-server.mjs`"" `
  -WorkingDirectory $d -WindowStyle Hidden `
  -RedirectStandardOutput "$d\bridge.launch.out.log" `
  -RedirectStandardError  "$d\bridge.launch.err2.log"
Start-Sleep -Seconds 3
```

### 3.3 扫描端口 / 查健康与连接状态

```powershell
for ($p=49620; $p -le 49629; $p++) {
  try {
    $r = Invoke-RestMethod -Uri "http://127.0.0.1:$p/health" -TimeoutSec 1 -ErrorAction Stop
    if ($r.service -eq "easyeda-bridge") { "BRIDGE PORT: $p"; $r | ConvertTo-Json -Compress; break }
  } catch {}
}
```

`/health` 返回示例：
```json
{"service":"easyeda-bridge","status":"ok","edaConnected":true,
 "edaWindowCount":1,"activeWindowId":"f0b54d7e-...","pendingRequests":0}
```

### 3.4 连接 EasyEDA
- 在 EasyEDA Pro 里安装扩展 **run-api-gateway.eext**：https://jlc-ext.com/item/oshwhub/run-api-gateway
- 加载后自动建立 WebSocket 连接；`edaConnected:true` 即就绪。

### 3.5 多窗口管理
```powershell
# 列出窗口
Invoke-RestMethod "http://127.0.0.1:49620/eda-windows"
# 选择窗口
Invoke-RestMethod -Uri "http://127.0.0.1:49620/eda-windows/select" -Method Post `
  -ContentType "application/json" -Body '{"windowId":"abc-123"}'
```
- 0 窗口：提示装/载扩展；1 窗口：自动选中；2+：需指定 windowId。

---

## 4. 执行代码（核心用法）

```powershell
$body = @{ code = "return await eda.dmt_Project.getCurrentProjectInfo();" } | ConvertTo-Json -Compress
Invoke-RestMethod -Uri "http://127.0.0.1:49620/execute" -Method Post `
  -ContentType "application/json" -Body $body -TimeoutSec 35
```

执行上下文（EDA 浏览器运行时）：
```javascript
async function(eda) {
  // 你的代码；eda 是全局 API 对象
  // 必须用 return 返回结果（console.log 不被捕获）
  // 所有返回 Promise 的方法必须 await
  // 浏览器环境，无 Node API（fs/path 等）
}
```
- 可指定窗口：body 加 `"windowId":"..."`。
- 默认超时 30s；复杂操作需拆分。
- PowerShell 传多行代码时，用 `($code -replace "`r`n"," ")` 压成单行更稳。

---

## 5. API 体系（模块前缀）

| 前缀 | 领域 | 代表类 |
|------|------|--------|
| `DMT_` | 文档管理 | Project, Board, Schematic, Pcb, EditorControl, Team, Folder, SelectControl |
| `PCB_` | PCB & 封装 | Document, Drc, Layer, Net, PrimitiveLine, PrimitivePad, PrimitiveVia, PrimitivePour, PrimitiveComponent, SelectControl |
| `SCH_` | 原理图 | Document, PrimitiveComponent, PrimitiveWire, SelectControl, Net |
| `LIB_` | 库 | Device, Footprint, Symbol, 3DModel, LibrariesList, Classification |
| `SYS_` | 系统 | Dialog, Message, Storage, FileSystem, IFrame, Window, Setting, I18n |
| `IPCB_` / `ISCH_` | 图元接口 | PrimitivePad, PrimitiveComponent, PrimitiveVia, PrimitiveWire... |
| `EPCB_` / `ESCH_` | 枚举 | LayerId, PrimitiveType, PadType... |

### API 文档位置
- `references/_index.md`：总索引
- `references/_quick-reference.md`：全方法签名速查
- `references/classes/`、`enums/`、`interfaces/`、`types/`：分类详情
- `format/`：工程/原理图/PCB 源文件格式参考（project/schematic/pcb）

### 常用只读方法（本项目审核实测可用）
| 方法 | 作用 |
|------|------|
| `dmt_Project.getCurrentProjectInfo()` | 当前工程信息（含所有 Board/原理图/PCB uuid） |
| `dmt_SelectControl.getCurrentDocumentInfo()` | 当前活动文档类型（documentType：3=PCB） |
| `pcb_PrimitiveComponent.getAll()` + `getAllPins()` | 元件（位号/层/坐标/封装）与焊盘 |
| `pcb_PrimitiveLine.getAll(net)` | 逐网络走线（线宽/层/端点） |
| `pcb_PrimitiveVia.getAllPrimitiveId()` | 过孔 |
| `pcb_PrimitivePour.getAll()` | 铺铜（网络/层） |
| `pcb_Net.getAllNets()` | 网络列表（含铜箔总长度 length） |
| `pcb_Drc.check(strict, userInterface, includeVerboseError)` | DRC 检查（返回错误数组，空=通过） |

---

## 6. 写操作范式（本项目当前策略：仅只读，不写）

> 本项目 PCB/原理图审核阶段**只读不改**。以下写法仅作能力记录。

- 创建走线：`pcb_PrimitiveLine.create(net, layer, sx, sy, ex, ey, lineWidth, lock)`
  - layer **必须用枚举** `EPCB_LayerId.TOP`，不要用裸数字。
- 修改图元（异步模式）：`get([id])` → `toAsync()` → `setState_X(...)` → `done()`。
- 原理图放器件：`sch_PrimitiveComponent.create({libraryUuid,uuid}, x, y, ...)`。
- 系统提示：`sys_Message.showToastMessage("...")`。

---

## 7. 关键坑 / 注意事项（SKILL 强调 + 实测）

1. **坐标单位不同域不同**（最常见错误）：
   - **PCB 单位 = 1mil**（1mm≈39.37 units）
   - **原理图单位 = 0.01inch = 10mil**（1mm≈3.937 units）
   - 用错会偏 10×。
2. **必须 await**：几乎所有 API 返回 Promise；忘记 await 拿到的是 Promise 对象。
3. **枚举不要猜**：层/类型等参数用枚举成员（如 `EPCB_LayerId.TOP`），查 `references/enums/`。
4. **文档状态**：操作前确认工程已打开、活动文档类型匹配（PCB API 需活动 PCB 文档）。
   - 新建工程后必须 `dmt_Project.openProject()` 才能操作文档。
5. **return 才有结果**，`console.log` 不被捕获。
6. **超时**：默认 30s；确认工程/文档已打开，复杂操作拆分。
7. **权限**：所有 API 受 EDA 权限系统控制；某 API 一致失败而其他正常，可能是权限/版本限制而非代码 bug。
8. **API 不存在就停**：`references/classes/` 未记载的方法不要臆造签名。
9. **扩展开发**：主进程禁用 `alert/confirm/localStorage/window/document`，改用 `eda.sys_*`；
   iframe 内直接用 `eda.xxx`，**不要** `window.parent.eda`。
10. **PowerShell 传参**：多行 JS 压成单行、`ConvertTo-Json -Compress`，避免转义问题。

---

## 8. 本项目实测记录（2026-08-24）

- Bridge 端口：**49620**；EDA 连接：1 窗口（`f0b54d7e-2828-4b89-86f4-064094cde24d`）。
- 当前工程：**AgenticKeyboard**（team techeditor）；含多个 Board：
  - `Expansion Baseboard_v1.0`（PCB `PCB_ExtBoard_v1.0`，uuid `a53dae1b12fd0cb8`）— 当前活动
  - 另有 v0.8 / v0.1 / v1.0_Bak 等历史版本 Board。
- 用途：对 `PCB_ExtBoard_v1.0` 做**只读审核**（59 元件 / 49 网络 / 193 过孔 / 双 GND 铺铜 / DRC 空错误）。
- 结论文档：`hardware/eda/pcb_review_report_2026-08-24.md`。

---

## 9. 会话资源管理

- Bridge 常驻消耗资源。若连续 3 次切到与 EDA 无关话题，可主动询问是否关闭 Bridge（不自动关）。
- 关闭 Bridge（Windows）：

```powershell
Get-NetTCPConnection -LocalPort (49620..49629) -State Listen -ErrorAction SilentlyContinue |
  Select-Object -Expand OwningProcess -Unique | ForEach-Object { Stop-Process -Id $_ -Force }
```

（SKILL 原文给的是 Linux `lsof/kill`，本机改用上面的 PowerShell 等价写法。）

---

## 10. 官方链接

- 网关扩展下载：https://jlc-ext.com/item/oshwhub/run-api-gateway
- 技能本体：`C:\Users\puzhang\.kiro\skills\easyeda-api-skill\`（含 `references/` 完整 API 与 `format/` 源格式）
