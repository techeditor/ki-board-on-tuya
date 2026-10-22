# firmware/app 安全性、可靠性与优化审计报告

- 审计日期：2026-09-14
- 审计对象：`/data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/app`
- 基准提交：`e4119dd85e32cf81ba367573f633d7a11eb62ccf`（`feat: add LCD device settings page`）
- 审计方式：源码静态审计、配置交叉核对、现有 Host 单元测试；未修改任何固件源码
- 审计限制：未执行 T5AI 目标构建、BLE 动态攻击测试、真机并发压力测试或云端故障注入

> 快照说明：审计过程中检测到另一个会话正在修改以下 5 个文件：
> `firmware/app/common/include/ak_board_io.h`、`ak_gfx.h`、`ak_statuslcd.h`、
> `firmware/app/common/src/ak_gfx.c`、`ak_statuslcd.c`。本报告未触碰这些改动；
> 与它们有关的结论按 2026-09-14 02:55 UTC 左右读取到的工作区版本记录，后续修改后应复核。

## 1. 执行摘要

当前程序已经具备较好的基础防护：大量字符串使用 `snprintf`，ASR 数据复制有长度上限，
RX 超长行会整行丢弃，绘图矩形有边界裁剪，云初始化失败会退化到普通键盘而不是继续调用
`ai_chat_init`，LCD 也被安排在云栈前初始化。现有 Host 单测全部通过。

但在量产或公开演示前，仍有三类优先问题：

1. **BLE 控制面没有应用层鉴权或敏感命令门控**。附近客户端一旦连接并写入 GATT，便可尝试下发
   Wi-Fi、License、显示和语音路由命令；Wi-Fi 密码和 AuthKey 也经这条通道传输。
2. **JSONL 输出未做 JSON 转义，且异步回调直接与主循环并发写同一传输**。这会造成协议注入、
   丢帧、串帧、重复序号或 Companion 误解析。
3. **DUAL 模式将 UART 与 BLE 输入合并进同一个行解析器**，BLE 环形缓冲又会按字节丢旧数据且
   断线不清空，存在跨链路、跨连接拼接命令的风险。

本次没有发现能够仅凭静态证据确认的直接栈溢出或任意代码执行漏洞。第一轮发现 3 项高风险、
12 项中风险、7 项低风险/维护问题和 8 组优化机会；第二轮 5 路交叉复核后，最终合计为 **3 项高风险、
15 项中风险、9 项低风险/维护问题，以及 9 组明确优化机会**。

### 1.1 第二轮交叉复核增补

以下项目由 5 个并行只读审计结果提出，并由主审计再次读取当前源码或 TuyaOpen 类型定义后确认。

#### M-13 GATT write 未绑定预期 characteristic 和当前连接

- `common/src/ak_ble_gatt.c:138-142` 的 TAL 路径只检查数据指针/长度；write report 实际携带 peer/connection 信息。
- `common/src/ak_ble_gatt.c:230-234` 的 TKL takeover 路径同样未检查 `event->conn_handle`，也未检查
  `write_report.char_handle`。上游 `tkl_bluetooth_def.h` 明确定义这两个字段。
- 在 takeover 使用平台全局 GATT callback 时，应用应明确拒绝非当前连接、非预期 RX characteristic 的 write，
  不能仅假设 SDK 永远只把目标 characteristic 交给该 callback。

**建议**：保存实际 RX characteristic handle；write 时同时验证连接、characteristic、结果码和允许的最大长度。
该问题归为中风险；H-01 已覆盖“未鉴权敏感命令”的主要攻击链，不重复升级为 Critical。

#### M-14 transport/input 初始化失败会永久退出 app task

**整改状态（2026-09-14）**：已修复。transport、input 和 TX mutex 初始化各重试 3 次；仍失败时延迟 2 秒后
调用 `tal_system_reset()`，且 reset 异常返回时保持 task 存活。app thread 创建同样重试 3 次并采用受控重启，
不再静默返回。

`src/tuya_app_main.c:1204-1212` 在 transport 或 input 初始化失败后直接 `return`；
`app_thread` 随后在 `:1423-1428` 删除自己，没有重试、降级模式、错误屏或受控重启。
这会让设备仍上电但表现为完全失效。

**建议**：定义明确 fatal policy：有限次数退避重试；仍失败时显示错误码并受控重启，或在可行时进入降级模式。

#### M-15 `CONFIG_DISPLAY_NAME` 与实际运行设备名不一致

所有 profile 和 `app_default.config` 都设置 `CONFIG_DISPLAY_NAME="ak_lcd"`；实际产品路径在
`common/src/ak_statuslcd.c:17,167,172` 注册并查找 `"ak_statuslcd"`。当前代码因直接使用硬编码名字仍能工作，
但 SDK 组件或后续代码按配置查找时会失败。

**建议**：统一配置与 runtime 名称，并移除旧 `ak_display` 命名残留。

#### L-08 reset counter KV 长度和取值未严格校验

`common/src/ak_reset_netcfg.c:30-31` 只要求 `read_len > 0` 就读取第一个字节；未要求长度恰为 1，
也未把异常值钳制到阈值。不会越界，但损坏/旧版本 KV 可能意外触发网络解绑。

**建议**：只接受 `read_len == sizeof(uint8_t)`，异常记录按首次启动处理并钳制计数。

#### L-09 固定 BLE 名称且持续 General Discoverable，存在可跟踪性

`common/src/ak_ble_gatt.c:30,55-61` 固定广播 `AK-KB`，并持续使用 General Discoverable；应用内未配置
RPA/LE Privacy。设备容易被长期识别和定位，也让开放控制面更容易被发现。

**建议**：配网/首次绑定后缩短 discoverable 窗口；平台支持时启用 RPA，或至少在已绑定状态降低可识别信息。

#### O-09 `link_text` 每次计算但从未显示

`src/tuya_app_main.c:912-925` 调用 `ak_link_header` 生成 `link_text`，最终标题却固定传入
`"Ki-board for Kiro Crew"`，只有颜色被使用。该计算和相关注释已与当前 icon 标题设计脱节。

**建议**：若状态由 icon 表达，就让 helper 只返回颜色；若仍需要文字状态，则真正绘制 `link_text`。

### 1.2 交叉复核中未采纳的结论

- **未采纳“缺少 watchdog feed = 高风险”**：当前证据不能证明该 profile 已启用且要求 app task 主动喂狗；保留为真机测量项。
- **未采纳“`ak_display.c` 必然造成最终 Flash 大幅增加/双注册”**：它确实被编入静态库源列表且没有调用，属于 O-03；
  但最终链接器可能不拉入未引用 object，不能在没有 map 文件的情况下声称一定进入 BIN 或执行注册。
- **未采纳“TAL 没显式 notify handle = 高风险”**：`tal_ble_server_common_send` 本身就是 common-service 抽象；
  诊断字段不对称值得整理，但不能据此推断会发给错误设备。
- **未采纳“BLE 三项 Critical”重复计数**：无鉴权、敏感命令和凭据传输是一条攻击链，统一保留为 H-01，
  适用条件仍是近距离无线访问，而不是无需条件的远程代码执行。
- 一名子审计代理越过 `firmware/app` 范围读取了本地凭据记录文件；主审计没有复制、引用或写入其中任何值，
  该越界内容不作为本报告发现。后续审计应继续严格禁止读取真实凭据文件。


## 2. 范围与验证

### 2.1 已检查

- 主状态机与 UI：`src/tuya_app_main.c`
- 协议与输入：`ak_protocol.c`、`ak_rx.c`、`ak_uart.c`、`ak_input.c`、`ak_input_fsm.c`
- BLE：`ak_ble_gatt.c`
- 云与语音：`ak_cloud.c`、`ak_asr.c`、`ak_wifi.c`、`ak_reset_netcfg.c`
- 显示与设置：`ak_statuslcd.c`、`ak_gfx.c`、`ak_display.c`、`ak_settings.c`
- 构建与配置：`CMakeLists.txt`、`Kconfig`、`app_default.config`、`config/*.config`
- Host 测试与 README
- 上游 TuyaOpen 当前 BLE service 默认版本定义（用于核对 0x1910 与 0xFD50）

### 2.2 已执行验证

```text
== ak host tests ==
test_short_press
test_long_press
test_debounce_bounce
test_dclick
test_settings
test_voice_route
test_protocol
test_rx
test_status_icons
ALL TESTS PASSED
```

Host 测试仅覆盖纯 C 逻辑，不覆盖 BLE 安全、异步回调、UART/BLE 并发、SDK 初始化失败、
PSRAM/LCD、云连接及真实音频状态机。

### 2.3 完整构建与未执行项

- 2026-09-14 后续修复回合执行 `tos.py build` 已成功：重新编译全部变更 app 对象、链接 `libtuyaapp.a`，
  完成 T5AI AP/CP 链接并生成 `app_QIO_0.1.2.bin`、`app_UA_0.1.2.bin` 和 `app_UG_0.1.2.bin`。
  先前的 `libbk_log.a` 缺失和 `libbk_vfs.a` 格式错误未再出现，无需删除共享平台缓存。
- 未连接实体板：无法验证 BLE 链路是否实际启用加密、SDK 回调线程模型、LCD 刷屏耗时及云重连行为。
- 未读取真实 `ak_tuya_authkey.h`，也未回显 UUID/AuthKey。

## 3. 高风险问题

### H-01 BLE 控制面缺少可见的鉴权、加密强制与敏感命令门控

**MVP 决策（2026-09-14）**：严重度保持 High，但当前仅限受控内部测试，整改优先级由 P0 下调为 P2；
在外部试用、公开演示或处理真实生产凭据前必须重新提升为发布阻断项。

**证据**

- `common/src/ak_ble_gatt.c:138-142` 和 `:230-234`：任意收到的 GATT write 数据直接进入 RX 环形缓冲。
- `src/tuya_app_main.c:1083-1180`：进入同一个 `on_rx_line` 命令分发器。
- `src/tuya_app_main.c:1132-1163`：可调用 Wi-Fi 配置和 License 写入。
- `src/tuya_app_main.c:1111-1131`、`:1165-1180`：可改变语音路由和屏幕内容。
- 在 `firmware/app` 内未发现 pairing、bonding、link encryption、challenge、token、nonce、HMAC、
  allowlist 或物理确认门控。

**触发条件**

附近设备发现 `Agentic Keyboard-XXXXXX`，建立 BLE 连接并向可写特征发送 JSONL。即使底层 SDK 默认支持 BLE 安全，
当前应用代码也没有显式要求“只有已绑定且加密的连接”才能执行敏感命令。

**影响**

- 非授权修改 Wi-Fi 配置，造成断网或把设备接入攻击者控制的 AP。
- 覆盖设备 License，导致云身份失效、串号或设备被攻击者账号接管。
- 伪造 Kiro Crew 状态、改变 ASR 路由、骚扰 UI。
- Wi-Fi 密码和 AuthKey 可能在未被应用层强制保护的 BLE 链路上传输。

**建议**

1. BLE 层显式要求加密 + 已绑定连接，并核对当前连接的安全级别。
2. 增加设备级配对密钥或 challenge-response；不要仅依赖设备名/服务 UUID。
3. `wifi`、`license` 仅在短时 provisioning mode 内开放，进入该模式需物理按键长按或本地 USB 授权。
4. 已存在有效 License 时默认拒绝远程覆盖；覆盖需物理确认或恢复出厂流程。
5. 对每个命令按 transport、连接身份和设备状态做 allowlist，不要让 BLE write 自动拥有 USB 同等权限。
6. 增加失败计数、速率限制和审计日志，但日志绝不记录密码/AuthKey。

### H-02 JSONL 输出未转义，存在协议注入和合法数据丢失

**整改状态（2026-09-14，固件 0.1.2）**：已修复。所有动态字符串通过统一有界 JSON writer 转义；
ASR 使用 2 KiB 静态 TX 缓冲，覆盖 255 字节输入按 `\u00xx` 最坏六倍展开，避免占用 callback 栈；发送函数按实际
buffer capacity 检查并记录超长错误。BLE notify 按协商 MTU 分片，Companion 继续按换行聚合完整 JSONL。
Host 回归测试覆盖最坏 255 字节控制字符输入，完整 T5AI 构建通过。

**原始证据**

- `common/src/ak_protocol.c:32-35`：ASR 文本直接插入 `"text":"%s"`。
- `common/src/ak_protocol.c:51-56`：SSID 直接插入 `"ssid":"%s"`。
- 其他通用 builder 同样直接插入 `%s`。
- `common/include/ak_protocol.h:21-23` 声明调用者必须先提供 JSON-safe ASCII。
- `src/tuya_app_main.c:296-308` 的 `on_asr_text` 未做 JSON escaping/sanitize。
- `common/src/ak_asr.c:51-60` 最多复制 255 字节 ASR 文本。
- `src/tuya_app_main.c:125`、`:194-199` 输出帧缓冲仅 192 字节；`snprintf` 返回值达到上限时整帧静默丢弃。

**触发条件**

ASR 文本、SSID 或未来任何动态字段包含 `"`、`\\`、换行、回车、控制字符，或 ASR 文本长度使完整帧超过
191 字节。

**影响**

- 换行可提前结束当前 JSONL，再构造下一条伪造帧，导致 Companion 将同一输入当成多个协议消息。
- 引号/反斜杠会生成非法 JSON，消息被 Companion 丢弃。
- 较长但合法的识别文本会完全消失，用户看见 ASR 成功但 PC 收不到结果。
- SSID 可造成日志/协议显示混乱。

**建议**

- 实现统一的 JSON string encoder，正确处理引号、反斜杠、控制字符和 UTF-8；所有 `%s` 动态字段统一调用。
- builder 返回结构化状态：成功、截断、非法输入；禁止调用者忽略截断。
- 将 ASR 协议缓冲提升到与上限一致，或设计分片/长度前缀；不要仅扩大栈数组后继续无界增长。
- 为 `"`、`\\`、`\n`、UTF-8、191/192/255 字节边界建立测试。

### H-03 多线程直接写同一协议传输并共享非原子序号

**整改状态（2026-09-14，固件 0.1.1）**：MVP 修复已完成。Tuya mutex 现在覆盖序列号分配、完整帧构建和
transport write，同一临界区保证序列号唯一且按发送顺序递增；无序号帧也使用同一 TX mutex。
长期仍可按建议迁移到单写者事件队列，以隔离 callback 阻塞。

**原始证据**

- `common/include/ak_asr.h:33` 明确 ASR callback 运行在 ASR/event thread。
- `src/tuya_app_main.c:296-340`：ASR 和 Wi-Fi callback 直接递增 `s_sequence` 并调用 `transport_write_frame`。
- `src/tuya_app_main.c:223-234`、`:282-293`、`:396-413`：app thread 也递增同一 `s_sequence` 并写 heartbeat、按键、voice、ack。
- `s_sequence` 不是 atomic，也没有 mutex；UART/BLE write 也没有应用层串行化。

**触发条件**

ASR 结果、Wi-Fi 状态变化与按键、heartbeat、ack 在相近时间发生。

**影响**

- `s_sequence++` 丢更新、重复序号或乱序。
- 两个线程同时写 UART 时可能字节交织，破坏 JSONL 边界。
- BLE notify API 若不支持多线程重入，可能失败或访问不一致连接状态。
- 在 C 内存模型下，未同步共享读写属于数据竞争，行为不可依赖 `volatile` 修复。

**建议**

- 所有 callback 只投递结构化事件到 app thread 队列；只有一个协议 TX owner 负责分配序号、编码和写传输。
- 如果短期无法重构，至少用 mutex/critical section 保护序号与完整帧写；但单写者队列仍是更稳妥方案。
- 给队列定义容量、优先级、丢弃策略和指标；License/Wi-Fi 状态不应被 heartbeat 挤掉。

## 4. 中风险问题

### M-01 入站解析器不是完整 JSON parser，缺少 schema/version 验证

**证据**：`common/src/ak_rx.c:46-108` 通过 `strstr` 找第一个 `"key"`，不验证对象边界、payload 层级、
重复 key、尾部垃圾、JSON 完整性或协议版本；字符串超出目标数组时会截断但仍返回成功。
`src/tuya_app_main.c:1083-1180` 仅凭 `type` 执行命令。

**影响**：形似 JSON 的垃圾文本、重复字段、错层字段也可能被接受；长 UUID/AuthKey/SSID 被静默截断后仍执行。

**建议**：使用受限但真正的 JSON parser，验证 `v == 1`、顶层 type、payload schema、字段唯一性、严格长度和枚举值；
解析失败返回 NACK/error code。

### M-02 DUAL 模式把 UART 和 BLE 字节流合并进同一个行解析器

**证据**：`src/tuya_app_main.c:74-84` 明确 UART 优先、无 UART 数据时读取 BLE；`:1400` 两者均喂给同一个 `s_rx`。
代码注释假设“PC exactly one link at a time”，但程序没有强制这一前提。

**影响**：UART 的半条消息可与 BLE 的后半条拼成一行；附近 BLE 客户端可污染 USB 会话的 parser state，反之亦然。

**建议**：UART 和 BLE 各自维护独立 `ak_rx_reader_t`，完整解析后再进入统一命令队列；敏感命令还需记录来源并执行权限检查。

### M-03 BLE 环形缓冲在满时由 producer 修改 consumer tail，且断线不清空残留

**证据**：`common/src/ak_ble_gatt.c:67-76` 缓冲满时 callback 直接推进 `s_rx_tail`；app thread 同时也修改 tail。
`:146-150`、`:181-189` 断线只改 link state，没有清空 ring；主 `ak_rx` 的半行同样继续保留。

**影响**：违反原本 SPSC 的单一所有者假设，可能丢字节、重排或产生不一致；上一连接的残余字节可与下一连接拼接。
按字节丢最旧数据还可能把一条消息的后缀保留下来并交给宽松 parser。

**建议**：使用 SDK 安全队列或带内存屏障的 ring；满时丢整帧/整包而不是单字节；断线时清 ring 和该 transport 的行 parser；
记录 overflow 计数并主动 NACK/重同步。

### M-04 ASR UI 使用单槽 `volatile` mailbox，可能覆盖结果且不具备内存同步语义

**证据**：`src/tuya_app_main.c:181-183` 单个文本槽 + 两个 volatile flag；`:993-1015` callback 覆盖槽位，app thread 消费。

**影响**：第二个 ASR/error 事件在第一个被消费前到达时会覆盖前一个；`volatile` 不等价于跨线程 release/acquire。
屏幕历史与实际发送到 Companion 的事件可能不一致。

**建议**：复用统一 app event queue；至少使用 SDK mutex/queue，并定义连续 ASR 事件的去重/排队规则。

### M-05 云初始化和 IoT task 忽略多个关键返回值

**证据**：`common/src/ak_cloud.c:114-121` 忽略 KV、timer、workq、time service、authorize 初始化结果；
`:159-170` 忽略 `netmgr_init` 和 netcfg 设置结果；`:197-205` 忽略 `tuya_iot_start`/`tuya_iot_yield` 返回值并永久循环。

**影响**：底层初始化失败后仍设置 `s_iot_ready`，后续表现为“初始化成功但永不上云”；如果 `yield` 在错误状态快速返回，可能形成忙循环。

**建议**：逐步检查并传播错误；区分 fatal/degraded；`tuya_iot_start` 失败应退避重试或终止 task；yield 异常需限速、计数和 watchdog 策略。

### M-06 运行时写入新 License 后，当前 IoT client 仍使用旧身份

**证据**：`common/src/ak_cloud.c:130-151` 仅在 `ak_cloud_init` 时读取 License 并构造 client；
`common/src/ak_wifi.c:141-152` 后续 `tuya_authorize_write` 只写持久存储；`src/tuya_app_main.c:1145-1163` 立即报告 written，未重启或重建 client。

**影响**：持久化身份与当前运行连接不一致；用户看到“written”却仍以旧身份运行，重启后才变化。

**建议**：协议明确返回 `written_reboot_required`，在安全确认后受控重启；或停止云 task、销毁并按新 License 重建 client。

### M-07 “纯 BLE GATT + cloud ASR”组合的初始化顺序不完整

**证据**：程序注释和 README 宣称 cloud ASR 独立于 transport；纯 BLE 时 `ak_transport_init` 在 cloud 前调用 `tal_ble_bt_init`。
`common/src/ak_cloud.c:159` 的 `netmgr_init` 可能再次接管 BLE callback；只有 `src/tuya_app_main.c:1230-1255` 的 DUAL 分支会在 cloud 后 takeover。

**影响**：若未来建立 BLE_GATT + cloud 非 DUAL profile，BLE callback/service handle 可能被 netmgr 覆盖，连接或 notify 失效。

**建议**：把“云先初始化、再 takeover BLE”的规则抽成 transport lifecycle，而不是只写在 DUAL 条件分支；为每个允许组合建立构建矩阵。

### M-08 BLE HID 是可选择配置，但产品 app 没有对应 backend

**证据**：`Kconfig` 暴露 `AK_TRANSPORT_BLE_HID`；`CMakeLists.txt:53-54` 会定义该宏；
`common/include/ak_features.h:39-40` 把它列为合法 transport；但 `src/tuya_app_main.c:29-114` 没有 HID 分支，最终进入 `#error`，
`CMakeLists.txt` 也未加入产品版 `ak_ble_hid.c`。

**影响**：配置看似支持，实际产品 app 无法构建 HID profile。

**建议**：在产品 app 正式接入 HID backend 前隐藏/拒绝该选项，或完整实现其无 JSONL 状态机；不要保留“可选但必失败”的配置。

### M-09 Kconfig 的 transport 不是互斥 choice，CMake 会静默按优先级选一个

**证据**：`Kconfig` 使用三个独立 bool；`CMakeLists.txt:49-54` 用 `if/elseif`，若误设多个，DUAL/GATT/HID 只取第一个。
编译期 guard 看见的是 CMake 生成后的宏，可能看不到原始配置冲突。

**影响**：错误 profile 可能构建成功但不是操作者以为的 transport。

**建议**：改为 Kconfig `choice`，DUAL 也作为 choice 成员；CMake 对多选/零选显式 `FATAL_ERROR`，并在构建日志打印最终矩阵。

### M-10 BLE 文档与广告仍写 V1 0x1910，但当前 SDK 默认注册 V2 0xFD50

**整改状态（2026-09-14）**：已修复。`AK_BLE_GATT_SERVICE_UUID` 与 `TAL_BLE_SERVICE_VERSION` 对齐；未显式覆盖时
与 TuyaOpen SDK 同样默认 V2 `0xFD50`，V1 构建则广播 `0x1910`。启动日志和公共头不再硬编码 V1，完整构建通过。

**证据**：`common/src/ak_ble_gatt.c:8-9`、`:54-57`，`common/include/ak_ble_gatt.h:6-20`、`Kconfig` 和
`src/tuya_app_main.c:1199` 都写 0x1910/0x2B10/0x2B11；当前上游
`/data/projects/TuyaOpen/src/tal_bluetooth/src/tal_bluetooth.c:37-38` 默认 `TAL_BLE_SERVICE_VERSION=2`，
`tal_bluetooth_def.h:25,39` 定义 V1=0x1910、V2=0xFD50。

**影响**：广播声明与实际 GATT 表不一致；依赖 service-filter 扫描的客户端可能看不到设备，维护人员会按错误 UUID 排障。

**建议**：广告跟随编译期 service version；公共头、Kconfig、日志和 README 同步写 V2，并保留清晰的 V1 fallback 说明。

### M-11 app thread 创建结果被忽略

**证据**：`src/tuya_app_main.c:1431-1440` 调用 `tal_thread_create_and_start` 不检查返回值。

**影响**：内存不足或线程创建失败时，固件入口静默返回，键盘不工作且没有明确故障状态。

**建议**：检查返回值并记录 fatal log；必要时安全重试、点亮错误状态或触发受控重启。

### M-12 生产二进制允许包含编译期 UUID/AuthKey fallback

**证据**：`include/tuya_config.h:13-18` 将 gitignored 头中的值编进固件；`common/src/ak_cloud.c:130-136` 授权区读取失败时回退到编译期值。

**影响**：源文件虽未提交，但发布的 BIN、构建缓存或物理 Flash 中仍含设备长期凭据；授权存储暂时读取失败时还可能意外使用旧 fallback 身份。

**建议**：生产 profile 禁止非占位 fallback，凭据只经授权工具写入设备安全存储；CI 扫描 BIN/strings 与构建产物访问权限；
调试 profile 与生产 profile 分离。

## 5. 低风险与维护问题

### L-01 `ak_json_uint` 溢出不报错

`common/src/ak_rx.c:97-108` 对超长十进制数执行 `uint32_t` 回绕。当前主要影响 ack seq，但应检查溢出并拒绝尾部非数字格式。

### L-02 广告启动和 MTU reply 多处忽略返回值

`common/src/ak_ble_gatt.c:82-91`、`:125`、`:212` 使用 `(void)` 忽略错误；`ak_ble_gatt_link_state` 在未连接时总返回 ADVERTISING，
即使广告实际启动失败。建议维护真实状态和重试退避。

### L-03 产品固件固定启用 DEBUG log，并打印 SSID

`src/tuya_app_main.c:1187` 使用 `TAL_LOG_LEVEL_DEBUG`，`:1142` 打印 SSID。没有打印密码/AuthKey，这是正确的；
但量产应降到 INFO/WARN，并对 SSID 的控制字符做日志转义。

### L-04 Settings 不持久化

`ak_settings_init` 每次启动恢复 volume=70、brightness=100。若这是产品设置，建议使用带版本和校验的 KV，采用延迟合并写入以避免每次按键磨损 Flash。

### L-05 LCD 初始化失败缺少清理/重入定义

`common/src/ak_statuslcd.c:151-193` 在 panel open 后若后续失败，没有关闭已打开设备；全局状态也没有 deinit/retry API。
当前只初始化一次，风险较低，但故障恢复会受限。

### L-06 README 严重滞后

`README.md:29-90` 仍称 v1 UART current、display pending、等待 v1.2 板，和当前 DUAL/cloud ASR、完整状态 UI、已完成真机阶段明显不一致。
这会误导构建和验收。

### L-07 产品没有 OTA 处理路径

`common/src/ak_cloud.c:63-106` 明确省略 OTA。若产品后续离开实验环境，漏洞修复只能物理重刷；应在产品化前设计签名校验、回滚和失败恢复。

## 6. 性能与架构优化机会

### O-01 改为脏区刷新，避免每次发送整帧

状态 LCD 使用 320×240×2 = **153,600 字节** PSRAM framebuffer。`ak_render_status` 每次状态变化都重画并 `ak_statuslcd_flush`；
SPI 48 MHz 下仅传输理论下限约 25.6 ms，尚未计命令、软件绘制和调度。启动阶段连续多次刷新，可能拉长启动并阻塞 5 ms 输入轮询。

建议：标题、sidebar、chat、status bar 分区 dirty；合并 20–50 ms 内多次状态变化；只 flush changed rectangle（若 TDL 支持）。

### O-02 减少逐像素/逐 run 的绘图 API 调用

`ak_gfx_char_impl` 对每个前景像素调用 `ak_gfx_fill_rect`；thin 字体和 icon 也有大量小矩形调用。建议直接写 framebuffer，
再一次提交脏区；预生成常用 glyph/icon 的 RGB565 mask。

### O-03 删除或隔离旧 `ak_display` 驱动

`CMakeLists.txt:24` 仍编译 `ak_display.c`，产品实际使用 `ak_statuslcd`。两套 ST7789 注册/初始化逻辑会增加维护分叉和误用风险。
若只为 bring-up 保留，应移回 bring-up app 或用明确 feature gate。

### O-04 cloud profile 不应无条件启用未使用的 LVGL

`app_default.config:12` 和 `config/TUYA_T5AI_CORE_dual_cloudasr.config:12` 启用 LVGL，但产品 UI 走直接 framebuffer，AI display 又被关闭。
链接器 GC 可能避免大部分 Flash 增量，但仍增加编译时间、依赖和配置复杂度。应实测 map 后关闭无用配置。

### O-05 所有 profile 都编译所有 backend 源码

`CMakeLists.txt:10-26` 总是加入 BLE、cloud、ASR、legacy display 等文件，再靠宏生成 stub/section GC。建议按 profile 条件化 source list，
降低编译时间、意外依赖和“未启用模块也能影响构建”的概率。

### O-06 评估 5 ms 全键轮询的功耗

app 每 5 ms 读取 10 个 GPIO，约 2,000 次 GPIO read/s，Wi-Fi 低功耗又在 cloud task 中关闭。当前交互延迟优秀，但常开设备功耗偏高。
可在不改变 20 ms debounce 的前提下评估 10 ms 扫描、idle 动态降频，或使用经过验证的 timer scan 服务。

### O-07 聊天历史每次刷新都重新 UTF-8 降级与换行

3 个 session 各保存 10×128 字节历史，每次 render 都重新 wrap。内存占用可接受（约 3.75 KiB），但可在写入历史时缓存已换行行，
减少重复 CPU 消耗。

### O-08 统一 transport/event 架构

建议形成三层：`transport RX per-link parser` → `validated command queue` → `single app state owner`；输出走 `single TX queue`。
这会同时解决 H-03、M-02、M-03、M-04，并为 USB/BLE 权限差异、统计和测试提供稳定边界。

## 7. 测试缺口

现有测试质量不错，但范围不足。建议新增：

1. **Parser 负向/模糊测试**：空行、超长行、尾部垃圾、重复 key、错层 payload、未知版本、数字溢出、非法 UTF-8、嵌入 NUL。
2. **JSON encoder 测试**：引号、反斜杠、CR/LF、控制字符、中文、emoji、最大长度和截断行为。
3. **DUAL 隔离测试**：UART/BLE 各发半行，证明不会互相拼接；断线后 parser/ring 必须清空。
4. **BLE ring 压力测试**：并发 producer/consumer、满缓冲、断线重连、不同 MTU、unsubscribe/resubscribe。
5. **权限测试**：未加密连接、未绑定设备、非 provisioning mode、已有 License 时，敏感命令必须拒绝。
6. **云故障注入**：KV、authorize、netmgr、IoT start、yield、audio codec、thread create 分别失败时验证退化路径。
7. **多线程 TX 测试**：ASR/Wi-Fi/按键/heartbeat 同时发生时每一行仍为完整 JSON 且 seq 唯一。
8. **构建矩阵**：UART、BLE GATT、DUAL、DUAL+cloud；HID 在产品接入前应明确 expected-fail 或隐藏。
9. **性能基线**：一次完整 LCD render/flush 耗时、app task stack high-water mark、cloud task stack high-water mark、RX overflow 计数。
10. Host 测试增加 ASan/UBSan，并对纯 parser 做持续 fuzz。

## 8. 推荐整改顺序

### P0：先封住信任边界

1. BLE 安全级别 + provisioning mode + 物理确认。
2. License 禁止无条件覆盖，严格校验 UUID=20、AuthKey=32（同时按 SDK 实际格式核实）。
3. 完整 JSON encoder/parser，所有字段 schema/长度/枚举严格验证。
4. 单一 TX owner，callback 只排队。

### P1：消除串帧和生命周期隐患

5. UART/BLE 独立 parser；BLE ring 改为安全队列，overflow 丢整帧，断线清状态。
6. 云初始化逐项检查返回值，并实现退避、错误状态和 watchdog 策略。
7. 修复/限制 BLE+cloud 和 HID profile，Kconfig 改互斥 choice。
8. 统一 SDK V2 0xFD50 文档、广告和诊断日志。

### P2：性能、测试和维护

9. LCD 脏区刷新、合并重绘、直接 framebuffer glyph。
10. 条件化源码和依赖，关闭未使用 LVGL，移除 legacy display 重复实现。
11. 补齐安全/并发/fuzz/故障注入/构建矩阵测试。
12. 更新 README 和产品化 OTA/凭据策略。

## 9. 已确认的正向设计

- `ak_rx_reader_feed` 对超长行采取“整行丢弃直到换行”，没有把截断前缀当完整行提交。
- `ak_gfx_fill_rect` 使用 32 位中间值并裁剪到 framebuffer 边界。
- ASR SDK 文本复制限制在 255 字节并显式补 NUL。
- 大部分协议 builder 使用 `snprintf`，调用点有统一 192 字节发送上限。
- 输入 FSM 的时间差使用无符号减法，对 `uint32_t` 毫秒计时回绕友好。
- Wi-Fi 日志只打印密码长度，不打印密码；License 也不回显真实值。
- LCD 在云/ASR 初始化前启动；cloud init 失败时跳过 `ai_chat_init`，避免已知空指针黑屏故障。
- Wi-Fi callback 不直接操作 framebuffer，而是发布 dirty flag 让 app thread 绘制。
- 三次快速重启只清网络/云绑定，设计上保留设备 License。
- Host 单测当前全绿，覆盖 debounce、long press、double click、settings、voice route、基础协议和 RX 拼行。

## 10. 结论

当前固件适合作为受控内部 MVP 测试。JSON 字符串编码和并发 TX 串行化已在 0.1.1 完成；BLE 敏感控制面
按当前风险接受暂缓，但在不可信无线环境、外部试用或真实生产凭据投入前仍必须整改。后续架构主线保持为：
**per-transport parser + validated command queue + single state/TX owner**。
