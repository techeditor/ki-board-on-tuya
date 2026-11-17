# 通信协议说明

> 协议的**权威定义**在 [`shared/protocol/protocol.md`](../shared/protocol/protocol.md)。
> 本文件仅作导读，避免两处内容漂移；实现请以 `shared/protocol/` 为准。

## 快速索引

- **载体**：阶段1 UART/USB-CDC（115200 8N1）→ 阶段2 BLE GATT → 阶段3 BLE HID
- **编码**：UTF-8 JSON Lines，每帧以 `\n` 结尾，未知 `type` 忽略
- **帧字段**：`v` / `seq` / `type` / `payload`
- **板→PC 事件**：`key`、`encoder`、`voice`、`macro`
- **按键手势**：K1–K6 和 EC11 按压每次物理操作上报一帧 `key`，`action` 为 `single` / `double` / `long`（互斥）；
  绑定了动作时带 `bind`（如 `chat1`）。语音键（默认 K5）上报原始 `down` / `up`。EC11 旋转走 `encoder` 帧。
  阈值：长按 800 ms，双击窗口 250 ms（第一次松开到第二次按下），`single` 在窗口到期时上报。
  完整语义、时序和 Companion 路由见权威源的 `key` 一节和 `firmware-key-gesture-events-spec.md`。
- **PC→板 命令**：`led`、`display`、`config`、`ack`
- **键盘设置（FR-02）**：`device_settings` 可写音量、亮度、语言、语音模式、灯光电源、自动断电时长与 LCD 字号（USB 或已配对 BLE），
  `device_settings_state` 另报 License / BLE 配对是否存在；敏感项只经 USB 且键盘弹⑦确认（`prov_open`、`ble_unpair`、
  `reset_netcfg`，或在⑦打开的配网窗口内的 `wifi` / `license` / `ble_pair`）。设置项清单见权威源 `device_settings` 一节。
- **LCD 菜单与按键灯光**（FR-04/06/07/08/09/10）：工作状态机（①待命 ②语音输入 ③输入确认 ④菜单 ⑤模型/模式菜单
  ⑥设置/测试 ⑦系统确认）；PC→板 `menu`（每页 ≤4 项、≤12 页、≤511 字节截断、`more` = 内容看电脑屏幕）、`menu_close`、
  `notice`、`menu_parked`（被 ESC 的请求数，K4 找回）；板→PC `menu_result`（选中项下标数组、口述答案 `text` 或 `dismissed`）。
  所有菜单 K2–K5 直选当前页、EC11 旋转跨页移动高亮 / 单击选高亮、K6 确认或多选提交（审批菜单无效）、K1 ESC。
  灯光：蓝可选、绿确认、红取消、黄注意/二次确认/等待、紫口述。
  keymap 动作 `model_menu`（K6 长按）/ `model_toggle`（K6 双击）。能力名 `menu`。详见权威源同名一节。
- **语音文字**：③ 的文字留在键盘上编辑，K6 的 `confirm` 帧带 `text`；Companion 默认经 Crew API 直接发到选中对话，
  失败时回 `input_restore` 让键盘回到 ③；可在 Companion 设置里改回注入 Crew 输入框。见权威源“语音文字的去向”。
- **握手/心跳**：`hello`/`hello_ack`、`ping`/`pong`

## 设计要点

1. 协议跨阶段复用：从有线到 BLE，`type`/`payload` 语义不变，只换载体。
2. 传输层与协议层解耦：固件与 Companion 各自实现可替换的 transport 后端。
3. 参照已克隆的外部参考项目（ki-board）的串口协议文档，减少重复设计。

详细字段、示例、状态机、配对鉴权见权威源文档。
