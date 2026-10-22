> 核实日期：2026-09-14
> 适用版本：Kiro Crew 0.6.0；Windows `kiro-cli-chat 2.21.4`
> 用途：说明 FP-4 Kiro Crew 状态回流的持久安装方式与真实验收步骤。

## 1. 两套 Hook 不要混淆

Kiro Crew 的 **Agent Capabilities → Hooks** 页面分为两部分：

- 上半区 **Hooks**：Crew 自己的全局 ScriptHookStore，持久化到
  `C:\Users\<user>\.kiro\crew\hooks.json`。
- 下半区 **ACP Agent Hooks**：CLI 2.x Agent JSON 内嵌 Hook。当前 bundled 审计
  Hook 位于此处；Companion 不修改这一部分。

CLI 3.0 的独立 v1 `.kiro/hooks/*.json` 格式不适用于当前 2.21.4。旧实现写入的
`~/.kiro/crew/hooks/ak-companion-status.json` 不会被当前运行时加载。

## 2. Companion 全局安装

点击 **Install globally** 后，Companion 在共享 `hooks.json` 的顶层 `hooks` 数组中
写入四条 ScriptHook：

| Crew Event | LCD 状态含义 |
|---|---|
| `AgentSpawn` | `running` / `agent started` |
| `UserPromptSubmit` | `received` / `prompt received` |
| `PostToolUse` | `tool` / `using <tool_name>` |
| `Stop` | `idle` / 回复摘要 |

每条 Hook 调用：

```text
"<absolute-path-to-companion.exe>" --hook
```

安装器仅管理名称以 `ak-companion-status-` 开头的条目，并遵循 Crew 的并发写入协议：

1. 获取同目录 `hooks.json.lock`；
2. 持锁重新读取并修改；
3. 保留现有手工 Hook（例如 `test`）以及所有顶层 webhook 注册键；
4. 重复安装幂等，并刷新 Companion 可执行文件路径。

外部程序修改文件后，运行中的 Crew 不会自动重载。请在 Kiro Crew 点击
**Apply & Restart**，或手工重启 Kiro Crew。

## 3. Crew Hook payload 兼容

Crew ScriptHookStore 使用 PascalCase 事件，并通过 STDIN 提供：

- `parent_session_key`：Companion 作为 Watched Chat 的 `session_id` 使用；
- `tool_name`：`PostToolUse` 当前工具；
- `assistant_text`：`Stop` 的完整回复文本。

Companion 同时兼容 CLI 风格的 camelCase、`session_id` / `sessionId`、
`assistant_response`，便于诊断和向后兼容。

## 4. Handoff 与 Status Sink

GUI 和短生命周期 Hook 进程通过 handoff 文件交换本次 Sink 的端口和临时 token：

- Windows：`C:\Users\<user>\AppData\Roaming\ak-companion\hook.json`
- 其他平台：`~/.config/ak-companion/hook.json`

点击 **Start sink** 创建 handoff；点击 **Stop sink** 或关闭 Companion 会删除它。
`hooks.json` 中不保存 token。Hook 子进程始终退出 0，避免影响 Agent turn，但会将
STDIN 解析、handoff、连接与 HTTP 错误写入 stderr，供 Crew 的 **Test** 和运行详情查看。

## 5. Windows 真实验收

1. 启动 Companion，点击 **Start sink**，确认 `LISTENING · 127.0.0.1:5477`。
2. 点击 **Install globally**，状态应为 `CONFIGURED · 4/4`。
3. 打开 `C:\Users\<user>\.kiro\crew\hooks.json`，确认四条
   `ak-companion-status-*` 与原有手工 Hook 并存。
4. 在 Kiro Crew 点击 **Apply & Restart**；回到 **Agent Capabilities → Hooks**，上半区
   应显示新增四条 Hook，下半区原 bundled ACP Hook 不变。
5. 对任一 AK Hook 点击 **Test**。Sink 已启动时应成功；失败时查看 stderr 中的
   `[AK-HOOK]` 诊断。
6. 在现有或新建 Kiro Crew 会话发送消息，确认 Companion 日志出现 `[KC-STATUS]`，
   Watched Chat 出现对应 `parent_session_key`。
7. 连接 AK 后确认 `display` 帧经 USB/BLE 更新 LCD。

手动 Sink 测试：

```powershell
$h = Get-Content "$env:APPDATA\ak-companion\hook.json" | ConvertFrom-Json
Invoke-RestMethod `
  -Uri "http://127.0.0.1:$($h.port)/status" `
  -Method Post `
  -Headers @{"X-AK-Status-Token"=$h.token} `
  -ContentType "application/json" `
  -Body '{"hook_event_name":"UserPromptSubmit","parent_session_key":"manual-test"}'
```

成功应返回 `ok`，并在 Watched Chat 显示 `manual-test`。

## 6. 验收清单

- [ ] `hooks.json` 安装后显示 `CONFIGURED · 4/4`，原手工 Hook 和 webhook 键保留。
- [ ] Apply & Restart 后 Crew 上半区显示四条 AK ScriptHook。
- [ ] 四类生命周期事件均到达 Sink。
- [ ] Watched Chat 显示 session、state 和 text。
- [ ] pin session 后其他会话不覆盖 LCD。
- [ ] USB 与 BLE 均能把状态显示到 AK LCD。
- [ ] Companion/Sink/板端断开并恢复后链路重新工作。
