# Ki-board：面向 Kiro 的 AI 小键盘（Tuya T5AI-Core）

基于 **Tuya T5AI-Core** 开发板的小型宏键盘，配合 Kiro 使用：语音输入直接送进
Kiro，键盘屏幕显示 Agent 状态。

*(English: [README.md](README.md))*

本仓库是 **v0.3.63** 版本的源码：固件 0.3.63、Companion 0.3.4、通信协议和硬件设计文件。

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

## 目录

```text
.
├── firmware/app/        # 产品固件（TuyaOpen，C）
├── firmware/assets/     # 固件位图的图标源文件
├── companion/           # 桌面端 Companion（Tauri：Rust + React/TS）
├── shared/protocol/     # 权威通信协议
├── docs/                # 协议导读、OTA、按键手势、版本规范
└── hardware/            # 引脚映射与设计导出
```

## 固件

需要 [TuyaOpen](https://github.com/tuya/TuyaOpen) 源码。

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE.config
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
cd src-tauri && cargo test   # 协议 / 传输单元测试
```

## 文档

| 文档 | 内容 |
|------|------|
| [shared/protocol/protocol.md](shared/protocol/protocol.md) | 权威协议规范 |
| [docs/protocol.md](docs/protocol.md) | 协议导读 |
| [docs/ota.md](docs/ota.md) | 本地 USB OTA：包格式、密钥与发布 |
| [docs/firmware-key-gesture-events-spec.md](docs/firmware-key-gesture-events-spec.md) | 按键手势事件 |
| [docs/versioning.md](docs/versioning.md) | 版本规范 |

## 许可

软件（固件、Companion、工具）采用 [Apache License 2.0](LICENSE)。`hardware/` 中的硬件设计文件
同时发布在立创开源平台（OSHWHub），沿用该项目页上注明的许可。字体等第三方内容沿用各自的许可证，
见 [NOTICE](NOTICE) 和 `firmware/app/third_party/fonts/THIRD_PARTY.md`。
