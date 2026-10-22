---
inclusion: always
---

# TuyaOpen · T5AI-Core 开发上下文

本项目（面向 Kiro 的 AI 小键盘）的固件基于 **Tuya T5AI-Core + TuyaOpen**。
开发固件 / 硬件 / 配网 / 语音相关任务时，**动手前优先查本地知识库
`/data/projects/tuya-kb/`**（独立于本仓库，仅供参考；内容对齐官方并标了核实日期）。
TuyaOpen SDK 位于 `/data/projects/TuyaOpen/`，同样独立于本仓库。

> 本 steering 由 `tuya-kb/.kiro/steering/tuyaopen-t5ai.md` 适配而来：
> 路径改为本仓库相对路径，并针对**本机 Windows 环境**调整工具链说明。

## 环境（本机 Windows）

- 本机是 **Windows**，不走 Linux 的 `apt-get` / `/dev/ttyACM` 那套。
- 工具链路线：**云编译 + 本地烧录**，见 `tuya-kb/03-toolchain-setup/01-cloud-build-local-flash-windows.md`。
- 板载串口芯片 **CH342F**（装 CH343SER 驱动）；烧录/日志用 TyuTool 或 `tos.py flash/monitor`。
- 本机 C 盘路径含 `@`/中文/空格已实测可用（见 `docs/internal/implementation-plan.md`），但 TuyaOpen 源码树
  建议仍放纯英文无空格路径以规避个别工具链问题。

## 关键事实（避免踩坑）

- CLI 工具是 **`tos.py`**（不是 `tos`）；每开一个终端先激活环境（Windows 云编译路线见上文档）。
- 选板子 `tos.py config choice`；menuconfig 是 `tos.py config menu`；改 `.config` 后必须 `tos.py clean -f` 再编译。
- 目标板 **T5AI-Core** = T5-E1 模组 / Cortex-M33F@480MHz / 8MB flash / 16MB RAM / Wi-Fi 2.4G + BLE5.4；
  板级配置 `TUYA_T5AI_CORE.config`；调试串口 Uart1 / 460800。
- 设备只连 **2.4GHz** Wi-Fi。UUID/AuthKey 是设备 License，**不要提交进仓库**。
- **USB 仅 Host 模式**，无 USB Device HID；"直连"只能走 **BLE HID**（见配网/蓝牙文档）。

## 知识库文档索引（均在 `/data/projects/tuya-kb/`）

- 框架总览：`/data/projects/tuya-kb/01-tuyaopen-framework/00-overview.md`
- T5AI-Core 硬件：`/data/projects/tuya-kb/02-t5ai-core-board/00-overview.md`；**引脚复用表**：`/data/projects/tuya-kb/02-t5ai-core-board/01-peripheral-pin-mapping.md`
- 环境搭建：`/data/projects/tuya-kb/03-toolchain-setup/00-environment-setup.md`；**Windows 云编译+本地烧录**：`/data/projects/tuya-kb/03-toolchain-setup/01-cloud-build-local-flash-windows.md`
- `tos.py` 全命令：`/data/projects/tuya-kb/04-tos-cli/00-tos-py-guide.md`
- 工程结构/编译：`/data/projects/tuya-kb/05-app-development/00-project-structure.md`
- 产品/PID/DP：`/data/projects/tuya-kb/06-dp-datapoints/00-product-and-dp.md`；DP C API：`/data/projects/tuya-kb/06-dp-datapoints/01-dp-c-api.md`
- 配网：`/data/projects/tuya-kb/07-network-provisioning/00-pairing.md`；**蓝牙/BLE HID**：`/data/projects/tuya-kb/07-network-provisioning/01-bluetooth-and-ble-hid.md`
- AI/音频总览：`/data/projects/tuya-kb/08-ai-audio-lcd/00-index.md`；音频/AI API：`/data/projects/tuya-kb/08-ai-audio-lcd/01-audio-ai-api.md`；
  **语音听写（取 ASR 文字）**：`/data/projects/tuya-kb/08-ai-audio-lcd/02-voice-dictation-and-events.md`；**LCD ST7789+LVGL**：`/data/projects/tuya-kb/08-ai-audio-lcd/03-lcd-st7789-lvgl.md`
- 授权/调试：`/data/projects/tuya-kb/09-debugging/00-auth-and-debug.md`；OTA：`/data/projects/tuya-kb/09-debugging/01-ota.md`
- 官方链接：`/data/projects/tuya-kb/10-references/00-official-links.md`
- **产品设计（无线小键盘/可行性/连接方案）**：`/data/projects/tuya-kb/11-product-design/`
- 器件手册（含 LCD / 44P 排针 / 摇杆等）：`/data/projects/tuya-kb/datasheets/`

## 与本项目文档的关系

- 硬件引脚分配以本仓库 `hardware/pinmap/` 为准；`/data/projects/tuya-kb/02-t5ai-core-board/01-peripheral-pin-mapping.md`
  与 `/data/projects/tuya-kb/datasheets/` 用于回填 44P 逐脚位号、CL28CK230 的 18P FPC 定义、摇杆脚序等待确认项。
- 协议以 `shared/protocol/protocol.md` 为准。
