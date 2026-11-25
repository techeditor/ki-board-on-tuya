# 参与 Ki-board 开发

感谢你关注 Ki-board：面向 Kiro Crew 用户的 AI 键盘。它把 Kiro Crew 的 agent 控制从屏幕移到指尖，
既是高效的提示词输入键盘，也是驱动 agent 的实体入口。目前只支持 Kiro Crew。

欢迎提交 bug 报告、功能建议、修复、翻译和文档改进。开 issue 或 PR 之前请先读完本页。
issue 和 PR 可以用英文或中文书写。

*(English: [CONTRIBUTING.md](CONTRIBUTING.md))*

## 报告 bug 和提功能建议

使用 GitHub 的 issue。先搜索未关闭和最近关闭的 issue，看是否已有人报告。一份有用的报告包含：

- 固件版本（`firmware/app/VERSION`）和 Companion 版本（`companion/VERSION`）；
- 你的板子、主机操作系统，以及键盘怎么连接（USB 或蓝牙）；
- 能复现问题的步骤，以及你期望的结果；
- 日志，其中的涂鸦产品 ID（PID）、UUID、AuthKey、Wi-Fi 密码和配对码要先删掉。

**安全问题不要提 issue**，见[安全](#安全)。

## 通过 PR 贡献

1. **先开 issue**：除了错别字和很小的修复，其它改动请先开 issue，确认方案后再动手。
2. **Fork** 仓库，从最新的 `main` 切分支。分支名 `<类型>/<简短主题>`，类型用完整英文单词：
   `feature/`、`fix/`、`docs/`、`refactor/`、`test/`、`build/`
   （例如 `feature/macos-key-injection`、`fix/ble-pairing-timeout`）。
3. **一次只做一件事。** 不要顺手格式化无关代码。不要修改版本号（`VERSION` 文件和清单文件里的版本字段），版本由维护者设置。
4. **构建并测试**你改动的部分（见[构建与测试](#构建与测试)），并在 PR 里写明跑了什么、在哪块板子或哪个系统上跑的。
5. **用 rebase 跟进 `main`。** 不要把 `main` merge 进你的分支，也不要留下 merge 提交。
   你的提交会原样被应用，所以每个提交都应该是有意义、自成一体的改动。
6. **向 `main` 提 PR**，填写说明（改了什么、为什么、怎么测的），并在问题解决之前保持跟进。

### PR 检查清单

- [ ] 关联了 issue（很小的修复除外）。
- [ ] 已 rebase 到最新的 `main`，没有 merge 提交。
- [ ] 提交信息符合[提交信息](#提交信息)。
- [ ] 相关的构建和测试通过，PR 里写明了你跑了什么。
- [ ] 没有凭证、PID、UUID、AuthKey 或固件二进制（见[什么永远不进 git](#什么永远不进-git)）。
- [ ] 改通信协议时，固件、Companion 和 `shared/protocol/protocol.md` 一起改。
- [ ] 行为变化时更新了文档。
- [ ] 新引入的第三方代码、字体、图片许可证兼容（见[许可证](#许可证)）。
- [ ] 没有改版本号。

### 你的 PR 之后会怎样

这个 GitHub 仓库是维护者源码仓库的发布快照，不是合并改动的地方。审查通过后，维护者在源码仓库里
应用你的提交（作者仍是你），改动随下一次发布的快照进入本仓库，快照的提交信息会署上你的名字。
之后你的 PR 会被关闭，并附评论指向包含该改动的快照。PR 是被关闭而不是用 GitHub 按钮合并，
因为快照会替换分支历史。我们会尽量回复每个 PR，但不承诺响应时间。

## 构建与测试

对你改动的部分跑下面的检查。仓库目前没有针对 PR 的自动检查，所以这些本地运行就是检查。

**Companion**（Node 22.x，见 `companion/.node-version`；Rust 工具链固定在 `rust-toolchain.toml`）：

```bash
cd companion
npm ci
npm run typecheck                     # tsc --noEmit
npm run frontend:build                # vite build
cd src-tauri && cargo check           # Rust 构建检查
```

`npm run tauri dev` 启动应用，`npm run tauri build` 打安装包。

**固件**（需要 [TuyaOpen](https://github.com/tuya/TuyaOpen) 源码）：

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config   # release profile
tos.py clean -f && tos.py build       # 改过任何 .config 之后必须先 clean
```

有两个 profile。`TUYA_T5AI_CORE_dual_cloudasr.config` 是 release profile；
`TUYA_T5AI_CORE_dual_cloudasr_debug.config` 只用于开发，debug 镜像不外传。
选 profile 可能改写 `app_default.config`：先看 `git status`，不要误带上这个文件。
详细说明和烧录见 [firmware/app/README.md](firmware/app/README.md)。

## Companion 的 macOS 与其它平台

Companion 目前在 Windows 上构建和测试，欢迎帮忙支持其它平台。平台相关代码用 `cfg(windows)` 或 `cfg(target_os = ...)` 隔离：

| 文件（`companion/src-tauri/`） | 平台相关部分 |
|---|---|
| `Cargo.toml` | `tauri`、`reqwest`、`keyring`、`cpal`、`native-tls` 只在 Windows 下依赖 |
| `src/lib.rs` | Tauri 应用本体（`mod app`、`run()`）只在 Windows 下编译，其它平台是桩 |
| `src/inject.rs` | 按键注入（Windows 用 `user32`），其它平台是桩 |
| `src/asr.rs`、`src/stream_asr/`（`capture.rs`、`model.rs`、`sherpa.rs`、`ws.rs`） | 系统语音输入、麦克风采集、模型下载、语音引擎加载 |
| `src/secrets.rs` | API key 存储（Windows 凭据管理器） |
| `src/kirocrew_control.rs` | Kiro Crew 窗口控制：Windows 有 Win32 后端，其它平台是桩 |
| `src/crew_identity.rs` | 判断本机 Kiro Crew 端口属于哪个进程：Windows 和 Linux 已实现，macOS 返回"未实现"（按失败处理） |
| `src/ble_pairing.rs` | 配对密钥存储：Windows 用 DPAPI，其它平台存在 `0600` 文件里 |
| `src/crew_api.rs`、`src/status_sink.rs` | `cfg(unix)` 的本地密钥文件、交接文件权限检查 |
| `tauri.conf.json` | 打包目标是 `nsis`（Windows 安装包） |

最新清单用 `git grep -n "cfg(.*\(windows\|target_os\|unix\)" -- companion/src-tauri` 查。
纯逻辑（协议、状态、存储）保持不含平台代码，这样在任何主机上都能测。
新增平台后端时，沿用现有代码的 trait 或函数接口，并保留其它平台的桩。PR 里写明你在哪个系统上测过。

## 硬件与文档

`hardware/` 是板子设计的导出文件。想改设计请开 issue 讨论，不要直接改导出文件。
`docs/` 下的文档以中文为主，README 是中英双语，代码注释用英文。

## 提交信息

提交信息用英文，Conventional Commits 风格：`<类型>(<范围>): <摘要>`。

- 现用类型：`feat`、`fix`、`docs`、`test`、`refactor`、`build`、`ci`、`chore`、`security`。
- 范围：`firmware`、`companion`、`hardware`，或组合如 `docs,firmware`。
- 例：`fix(firmware): BLE pairing window cannot be used up`、
  `feat(companion): read-only Device info block`、
  `docs: Device info page and the device_info frame`。
- 一个提交只做一件逻辑改动，测试和它覆盖的代码放在同一个提交里。

## 什么永远不进 git

- 任何凭证：API key、token、密码、证书。
- 涂鸦产品 ID（PID）、设备 UUID、AuthKey。仓库里 PID 是空的；
  自己的 PID 按 [firmware/app/README.md](firmware/app/README.md) 的说明写入。
- 固件二进制（`*.bin`、`*.elf`、`*.hex`）。带设备 License 编译的镜像含有该设备的
  UUID 和 AuthKey，文件一旦提交就留在历史里。`.gitignore` 已排除它们，不要 `git add -f`。
- 任何改动都不要写 flash 的 `0x7ce000` 区域（自定义开机画面）：这个区域在每个构建上是否空闲还没有核实。

## 许可证

固件、Companion、工具和文档使用 Apache-2.0（见 `LICENSE` 文件）。硬件设计文件沿用 OSHWHub 项目页上的协议。

- **你的贡献按项目许可证授权。** 提交 PR 即表示：改动是你写的或你有权提交，
  并且按它所改文件的许可证授权（软件和文档为 Apache-2.0，硬件文件为 OSHWHub 协议）。
  我们可能会请你确认贡献的授权。
- **第三方代码、字体、图片**必须能按 Apache-2.0 分发。宽松许可证（MIT、BSD、ISC、Apache-2.0，字体的 OFL）可以。
  Copyleft 许可证（GPL、LGPL、AGPL）、禁止商用或再分发的许可证、没有许可证的材料，
  除非维护者先在 issue 里同意，否则不接受。在 PR 里写明项目名和许可证，把它的许可证全文和署名跟它放在一起
  （字体见 `firmware/app/third_party/fonts/` 里的许可证全文），
  维护者会更新 `NOTICE`。

## 安全

如果你发现本项目有潜在的安全问题，请在仓库的 Security 标签页点 **Report a vulnerability**，私下报告。
**不要**创建公开的 GitHub issue、PR 或讨论。详见 [SECURITY.md](SECURITY.md)。
