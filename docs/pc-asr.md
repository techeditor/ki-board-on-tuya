# 电脑识别（PC ASR）

键盘的识别路由设为 `system`（Companion 设备页“语音识别 → 电脑识别”），或键盘自己的云端识别用不了（没有 License、没配网、云没连上，见下文“什么时候自动用电脑识别”）时，按住语音键说话，Companion 用电脑麦克风录音，
流式送到所选引擎，最终文字送回键盘的输入栏（K4 删字、长按清空，K6 发送），或者填进电脑上的 Crew 输入框（槽设了 Alt+N 时），与键盘云端识别（涂鸦）走同一条流程。
识别结果不会直接打进任何窗口。

代码：`companion/src-tauri/src/stream_asr/`（引擎、会话、模型下载）、`companion/src-tauri/src/secrets.rs`（钥匙串）、
`companion/src/ui/PcAsrPanel.tsx`（设置页）。

## 流程

1. 键盘：语音键按下发 `voice:start`（状态栏“正在听…”），松开发 `voice:stop`，停在 ②（状态栏“识别中…”）等结果。
   这要求 Companion 的 `hello` 带 `pc_asr:true`、固件的 `hello_ack` 带 `pc_asr_wait`；任一方是旧版时，松开后回 ①。
2. Companion：`start` 时打开默认麦克风（WASAPI，重采样到 16 kHz 单声道 16 bit），按所选引擎开始一次识别；
   中间结果只显示在 Companion 设置页的“试一试”框里。
3. `stop` 时结束录音，等最终结果（最多 8 s）。
4. 有文字：键盘模式的槽发 `input_restore`（`text` = 识别文字，`msg` = 引擎名，如 `电脑识别:豆包`，`src:"pc"`），文字追加到
   键盘的输入栏，状态栏绿色。电脑模式的槽（`voice:start` 的 `slot` 对应的对话设了 Alt+N）：Companion 切到那个对话，把文字打进
   Crew 输入框，不发送，再发 `input_restore` `filled:true`；失败时改按键盘模式处理。
5. 没文字或失败：发文字为空的 `input_restore`（`src:"pc"`，`msg` 为“没听清”或原因，如“电脑识别:没有 Key”），
   键盘回 ①，状态栏橙色。旧固件改发 `notice`。按住语音键期间就失败时，Companion 在 `voice:stop` 到达时再发一次。
6. 键盘等了 15 s 还没结果：回 ①，显示“警告：语音识别超时”。等待时 K1 单击取消（显示“已取消输入”）。两种情况键盘都发
   `voice:cancel`，Companion 丢弃这段识别，迟到的结果不会再出现。
7. 自动回退（默认开）：云端引擎没有 Key、连不上、Key 被拒、中途断线或 8 s 无结果时，把已录的音频整段交给本地引擎
   继续识别，状态栏显示“云端失败,已用本地识别”。只回退一次；本地模型未下载时直接报错。

## 什么时候自动用电脑识别

键盘自己的云端识别需要：涂鸦产品 ID 和 License 已写入、设备已激活并配网、Wi-Fi 和涂鸦云在线、AI 智能体就绪。
这些里任何一项不满足（没有 License、没激活或没配网、配网窗口开着、Wi-Fi 或云断线），按住语音键时固件自动改走电脑识别，
不需要在 Companion 里手动切换：

| 键盘状态 | Companion | 按住语音键 |
|---|---|---|
| 云端可用 | 任意 | 键盘云端识别 |
| 云端不可用 | 已连接（USB 或已认证蓝牙），`hello` 带 `pc_asr:true` | `voice:start`，走本文流程；状态栏写识别来源，如“电脑识别:本地” |
| 云端不可用 | 已连接，但 Companion 里“键盘控制电脑”关着 | Companion 回 `input_restore`（`src:"pc"`，`msg` 为“语音需要打开键盘控制电脑”），键盘不用等 15 s |
| 云端不可用 | 没连接 | 不发任何帧、不进 ②；状态栏“语音需要连接 Companion 或完成配网” |
| 云端不可用，语音模式「对话」 | 已连接 | 这一次改按“识别”处理（保存的模式不变），云恢复后自动回到对话模式 |
| 云端不可用，语音模式「对话」 | 没连接 | 同上一行“没连接”，仍留在对话模式 |

这条路径只用已有的帧（`hello`、`voice`、`input_restore`、`config`、`notice`），没有新增命令，也不放宽任何敏感命令的限制：
`wifi` / `license` / `license_read` / `reset_netcfg` / `ble_pair` 仍然只收 USB 来源并要求配网窗口打开，蓝牙仍需应用层加密认证。
配网窗口开着时语音照常工作，窗口的倒计时和写 License 的提示在语音状态提示消失后恢复显示。

所有路径都受“键盘控制电脑”开关（`control_enabled`）约束，与其他由键盘触发的电脑动作相同。

## 引擎

`AsrEngine` trait（`companion/src-tauri/src/stream_asr/mod.rs`）：`start`（结果回调）/ `push_audio` / `finish` / `cancel`，回调事件
`Partial` / `Final` / `Error`。三个实现：

| 引擎 | 接口 | 官方文档（2026-11-17 核对） |
|---|---|---|
| 豆包流式语音识别（双向流式优化版） | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`，二进制帧协议，200 ms 一包 | [大模型流式语音识别 API](https://www.volcengine.com/docs/6561/1354869) |
| 阿里云百炼 Fun-ASR 实时 / Paraformer 实时 v2 | `wss://dashscope.aliyuncs.com/api-ws/v1/inference`，`run-task` → 音频 → `finish-task`，100 ms 一包 | [WebSocket 接入](https://help.aliyun.com/zh/model-studio/fun-asr-realtime-websocket-api)、[客户端事件](https://help.aliyun.com/zh/model-studio/fun-asr-client-events)、[服务端事件](https://help.aliyun.com/zh/model-studio/fun-asr-server-events)、[Paraformer 实时](https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service) |
| 本地 sherpa-onnx | 流式 Paraformer 中英双语 int8，运行时动态加载 sherpa-onnx C API | [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)、[模型](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en) |

鉴权：豆包新版控制台只用 `X-Api-Key`；旧版控制台用 `X-Api-App-Key`（App ID）+ `X-Api-Access-Key`（Access Token），
设置页填了 App ID 就按旧版发。`X-Api-Resource-Id` 在设置页选（默认 2.0 小时版）。百炼用 `Authorization: Bearer <API Key>`。

热词：豆包放在 `request.corpus.context`（`{"hotwords":[{"word":…}]}`）；百炼 Fun-ASR 放在 `input.context`
（user / `input_text`）。Paraformer 只认预先创建的 `vocabulary_id`，本地流式 Paraformer 不支持热词，这两种不使用热词。

## 本地模型

- 模型：`csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en`，Hugging Face 版本固定为
  `8e40c43232a1c5c66c82111efc5820d3accca11b`，取 `encoder.int8.onnx`（165 MB）、`decoder.int8.onnx`（72 MB）、
  `tokens.txt`。来源 FunASR `speech_paraformer_asr_nat-zh-cn-16k-common-vocab8404-online`。
- 运行库：sherpa-onnx v2.0.1 Windows x64 共享库（`sherpa-onnx-c-api.dll`、`onnxruntime.dll`、
  `onnxruntime_providers_shared.dll`），来自 GitHub Release，下载 7.9 MB，解压约 22 MB。
- 合计下载约 245 MB，放在 `%USERPROFILE%\.ak-companion\asr-local\`。每个文件按写死的 sha256 校验，
  不符就删除并换下一个地址（模型另有 hf-mirror.com 镜像）。全部校验通过后才写 `verified.json`，设置页才显示“已就绪”。
- 不打进安装包；设置页可一键下载、取消、删除。只用作回退时第一次使用才加载（约 300 MB 内存）；默认引擎设为本地时启动即加载。
- 许可：模型权重 FunASR Model Open Source License 1.1（允许使用、复制、修改、分发，含商用，须保留来源和模型名）；
  转换仓库 Apache-2.0；sherpa-onnx Apache-2.0；onnxruntime MIT。
- 没选更小的 zipformer 中文模型：其训练集含 WenetSpeech / KeSpeech / AISHELL-2 等非商用数据，模型仓库也没有写许可证。

## API Key

- 只存在系统钥匙串：Windows 凭据管理器（`keyring` crate，目标名 `asr.doubao.Ki-board Companion` /
  `asr.aliyun.Ki-board Companion`）。macOS 构建时同一接口对应钥匙串（目前 Companion 只出 Windows 版）。
- 不写配置文件（`~/.ak-companion/asr.json` 只有引擎、回退开关、热词、App ID、资源 ID、模型名）、不进日志、
  不回传前端：前端只拿到“已保存 / 未设置”。错误信息不回显 Key。
- 代码里没有任何 Key；单元测试用 mock 服务器和假 Key。

## 新增依赖（都允许商用）

| crate | 许可证 | 用途 | 平台 |
|---|---|---|---|
| libloading 0.9 | ISC | 运行时加载 sherpa-onnx | 全部 |
| bzip2 0.6（libbz2-rs-sys 纯 Rust 后端） | MIT/Apache-2.0（bzip2-1.0.6） | 解压运行库 | 全部 |
| tar 0.4 | MIT/Apache-2.0 | 解包运行库 | 全部 |
| flate2 1（树里已有） | MIT/Apache-2.0 | 豆包 gzip 帧 | 全部 |
| native-tls 0.2（SChannel） | MIT/Apache-2.0 | wss 与 https | Windows |
| keyring 3 | MIT/Apache-2.0 | 凭据管理器 | Windows |
| cpal 0.18 | Apache-2.0 | 麦克风（WASAPI） | Windows |
| reqwest 加 `native-tls` 特性 | MIT/Apache-2.0 | 下载模型 | Windows |

间接依赖新增 55 个，许可证为 MIT、Apache-2.0、ISC、BSD-2-Clause、Zlib、bzip2-1.0.6 的组合（cpal/native-tls 的
Linux、macOS 后端也进了 `Cargo.lock`，但只在对应平台编译）。没有参考或拷贝另一个开源 ESP32 参考方案（`ref_projects/ki-board/`）的代码。

## 固件与协议

固件配合（改进 A，固件 0.3.66 起）：`hello` 的 `pc_asr`、`hello_ack` 的 `pc_asr_wait`、`input_restore` 的 `src`、
`voice` 的 `cancel`，细节见 `shared/protocol/protocol.md` 的 `voice` 和“语音输入：键盘模式与电脑模式”。发送失败的还原仍是红字。

仍有的限制：

1. 说话过程中的中间结果只在 Companion 窗口里，键盘屏幕上没有（改进 B：`asr_partial` 上屏，暂不做）。
2. 输入栏最多 380 字节（约 126 个汉字），和键盘云端识别相同，超出部分不记。
3. 云端失败后回退本地时，最长约 8 s + 本地识别时间；超过 15 s 键盘按超时处理。

## Windows 实测步骤

### 申请 Key

- 豆包：登录[火山引擎控制台](https://console.volcengine.com/speech/app)，进入“豆包语音”，开通“豆包流式语音识别模型”
  （2.0 或 1.0，小时版或并发版，记下开通的是哪个）。新版控制台在“API Key 管理”创建 API Key；旧版控制台创建应用后
  记下 APP ID 和 Access Token（Access Token 填进 Key 框，APP ID 填进 App ID 框）。说明见
  [文档的鉴权一节](https://www.volcengine.com/docs/6561/1354869)。
- 阿里云百炼：登录[百炼控制台](https://bailian.console.aliyun.com/)（华北2 北京），开通模型服务，在“API-Key”页面创建
  API Key（`sk-` 开头）。说明见[获取 API Key](https://help.aliyun.com/zh/model-studio/get-api-key)。

### 测试

1. 拉取分支，`cd companion && npm ci && npm run tauri dev`（或 `npm run build` 后安装）。
2. 设备页 → “语音识别”选“电脑识别”。键盘已连接时 Companion 会把路由同步给键盘。
3. 选“豆包”，粘贴 Key，按“保存 Key”，提示变为“Key 已保存在系统凭据管理器”。按“用电脑麦克风试一试”，说一句话，
   按“说完了”：框里先出现中间结果，再出现最终文字。
4. 同样测试“阿里云百炼”（Fun-ASR，可在热词里加一个生僻词对比）。
5. 选“本地离线”，按“下载本地模型（245 MB）”，等进度到 100% 并显示“已就绪”；断网后再试一试。
6. 用键盘（需本 MR 的固件和 Companion）：USB 连接，按住 K5 说话，状态栏“正在听…”；松开后显示“识别中…”，
   约 1 s 内文字出现在页面底部的输入栏，状态栏绿色“电脑识别:豆包”。K4 删一字（长按清空），K6 发到 K1–K3 选中的对话。
   - 等待中按 K1：回 ①，显示“已取消输入”，之后文字不会再出现。
   - 拔网线并关掉回退：约 8 s 后回 ①，橙色显示原因；什么都不说直接松开：显示“没听清”。
   - K6 发送失败（关掉 Crew）：文字回到输入栏，状态栏红字。
7. 回退：选“豆包”，删除 Key（或断网），按 K5 说话；状态栏应显示“云端失败,已用本地识别”，文字照常进输入栏。
   关掉回退开关再试，键盘显示“电脑识别:没有 Key”。
8. 安全检查：打开“凭据管理器 → Windows 凭据”，应看到 `asr.doubao.Ki-board Companion`；
   `%USERPROFILE%\.ak-companion\asr.json` 和 Companion 日志页里没有 Key，日志里也没有识别出的文字（只有字数）。
