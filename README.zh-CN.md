# Ki-board：面向 Kiro Crew 的 AI 小键盘（Tuya T5AI-Core）

一款基于 **Tuya T5AI-Core** 开发板、面向 Kiro Crew 用户的 AI 小键盘。它把 Kiro Crew
智能体的控制从屏幕搬到指尖：既是高效的提示词输入键盘，也是驱动智能体的物理入口。
核心差异化功能是**语音输入直达 Kiro Crew**与 **Agent 状态上屏显示**。目前只支持 Kiro Crew。

*(English: [README.md](README.md))*

本仓库是 Ki-board 固件、Companion、通信协议和硬件设计文件的源码。当前版本号见
`firmware/app/VERSION` 和 `companion/VERSION`。

## 硬件（v0.3）

| 部件 | 型号 |
|------|------|
| 主控 | T5AI-Core（T5-E1 模组，Cortex-M33F @ 480 MHz，Wi-Fi + BLE 5.4） |
| 屏幕 | 2.8" IPS LCD，ST7789P3，320×240，SPI |
| 按键 | 6 个机械键 K1–K6，热插拔座（兼容 MX） |
| 导航 | EC11 旋转编码器（A/B + 按压） |
| 指示灯 | 7 颗 WS2812 RGB |
| 音频 | 开发板自带麦克风和扬声器 |
| 扩展板 | 无源：只有按键、屏幕和灯；USB 与供电由开发板提供 |

引脚映射、网表、BOM 与冲突校验：[hardware/pinmap/](hardware/pinmap/)。
原理图、PCB、BOM 与 EasyEDA 导出：[hardware/exports/](hardware/exports/)。

## 架构

- **固件**（TuyaOpen，C）：按键和旋钮扫描、去抖 → 手势与状态机 → ST7789 屏幕和键灯，
  同一套协议跑在 USB 串口和加密的 BLE GATT 上。语音由板端经云端识别，或由电脑上的 Companion 识别。
- **Companion**（Tauri：Rust + React/TS）：传输后端和协议解析 → 按键、菜单和语音文字的路由。
  主路径是 Kiro Crew 本机接口（REST + WebSocket，只走本机回环，连接前先校验网关身份）：
  读取对话状态和请求，发送提示词、审批和回答，并把状态回写到键盘。
  向 Kiro Crew 窗口注入按键和 Kiro Crew 状态 hook 是后备。
  电脑识别用豆包、阿里云百炼两个云端引擎，或离线的 sherpa-onnx 模型。

Companion 目前在 Windows 上运行，macOS 支持在计划中。

详见 [docs/architecture.md](docs/architecture.md)。

## 目录

```text
.
├── firmware/app/        # 产品固件（TuyaOpen，C）
├── companion/           # 桌面端 Companion（Tauri：Rust + React/TS）
├── shared/protocol/     # 权威通信协议
├── docs/                # 架构概览、用户手册
└── hardware/            # 引脚映射与设计导出
```

## 固件

需要 [TuyaOpen](https://github.com/tuya/TuyaOpen) 源码。

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config
tos.py clean -f && tos.py build
```

详细说明（包括如何为云端 ASR 配置你自己的涂鸦产品 ID，即 PID）见
[firmware/app/README.md](firmware/app/README.md)。仓库里的 PID 为空。不要提交 PID、设备 UUID 或 AuthKey。

## Companion

需要 Node 22+ 和 Rust（版本见 `rust-toolchain.toml`）。

```bash
cd companion
npm ci
npm run tauri dev            # 运行
cd src-tauri && cargo check   # Rust 构建检查
```

## 文档

| 文档 | 内容 |
|------|------|
| [shared/protocol/protocol.md](shared/protocol/protocol.md) | 权威协议规范 |
| [docs/architecture.md](docs/architecture.md) | 架构概览 |
| [docs/user-manual.md](docs/user-manual.md) | 用户手册（[English](docs/user-manual.en.md)） |

## 参与贡献

欢迎提交 issue 和 PR，见 [CONTRIBUTING.zh-CN.md](CONTRIBUTING.zh-CN.md)（[English](CONTRIBUTING.md)）。
安全问题按 [SECURITY.md](SECURITY.md) 的说明报告，不要在公开 issue 里提。

## 许可

软件（固件和 Companion）采用 [Apache License 2.0](LICENSE)。`hardware/` 中的硬件设计文件
同时发布在立创开源平台（OSHWHub），沿用该项目页上注明的许可。字体等第三方内容沿用各自的许可证，
见 [NOTICE](NOTICE) 和 `firmware/app/third_party/fonts/` 里的许可证全文。
