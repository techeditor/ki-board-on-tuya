# Ki-board: Agentic Keyboard for Kiro Crew (Tuya T5AI-Core)

An AI keyboard for Kiro Crew users, built on the **Tuya T5AI-Core** board. It moves
Kiro Crew agent control from the screen to your fingertips: a fast keyboard for
prompts and a physical entry point for driving agents. Its differentiators are
**voice input straight into Kiro Crew** and **on-screen agent status**. It supports
Kiro Crew only.

*(中文版见 [README.zh-CN.md](README.zh-CN.md))*

This repository holds the source of the Ki-board firmware, the Companion, the
wire protocol and the hardware design files. The current versions are in
`firmware/app/VERSION` and `companion/VERSION`.

## Hardware (v0.3)

| Part | Model |
|------|-------|
| MCU | T5AI-Core (T5-E1 module, Cortex-M33F @ 480 MHz, Wi-Fi + BLE 5.4) |
| Display | 2.8" IPS LCD, ST7789P3, 320×240, SPI |
| Keys | 6 mechanical keys K1–K6, hot-swap sockets (MX-compatible) |
| Navigation | EC11 rotary encoder (A/B + push) |
| Indicators | 7× WS2812 RGB LEDs |
| Audio | on-board microphone and speaker |
| Expansion board | passive: keys, display and LEDs; USB and power come from the dev board |

Pin map, netlist, BOM and conflict checker: [hardware/pinmap/](hardware/pinmap/).
Schematics, PCB, BOM and EasyEDA exports: [hardware/exports/](hardware/exports/).

## Architecture

- **Firmware** (TuyaOpen, C): key and encoder scan with debounce → gesture and state
  machine → the ST7789 screen and the LEDs, with one protocol over USB serial and
  BLE GATT (encrypted). Speech is recognised on the board through the cloud, or on
  the PC by the Companion.
- **Companion** (Tauri: Rust + React/TS): transport backend and protocol parser →
  routing of keys, menus and voice text. The Kiro Crew local API (REST and
  WebSocket on loopback, gateway identity checked first) is the main path: it reads
  chat status and requests, sends prompts, approvals and answers, and reflects
  status back to the board. Key injection into the Kiro Crew window
  and Kiro Crew status hooks are the fallbacks. PC speech recognition uses Doubao
  and Alibaba Cloud engines, or an offline sherpa-onnx model.

The Companion currently runs on Windows; macOS support is planned.

Details: [docs/architecture.md](docs/architecture.md) (in Chinese).

## Repository layout

```text
.
├── firmware/app/        # product firmware (TuyaOpen, C)
├── companion/           # desktop Companion app (Tauri: Rust + React/TS)
├── shared/protocol/     # authoritative wire-protocol spec
├── docs/                # architecture overview, user manual
└── hardware/            # pin map and design exports
```

## Firmware

Needs a [TuyaOpen](https://github.com/tuya/TuyaOpen) checkout.

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config
tos.py clean -f && tos.py build
```

Details, including how to set your own Tuya product ID (PID) for cloud ASR:
[firmware/app/README.md](firmware/app/README.md). The repository ships with an
empty PID. Never commit a PID, device UUID or AuthKey.

## Companion

Prerequisites: Node 22+, Rust (see `rust-toolchain.toml`).

```bash
cd companion
npm ci
npm run tauri dev            # run the app
cd src-tauri && cargo check   # Rust build check
```

## Documentation

| Doc | Content |
|-----|---------|
| [shared/protocol/protocol.md](shared/protocol/protocol.md) | Authoritative protocol spec |
| [docs/architecture.md](docs/architecture.md) | Architecture overview |
| [docs/user-manual.en.md](docs/user-manual.en.md) | User manual ([中文](docs/user-manual.md)) |

## Contributing

Issues and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md)
([中文](CONTRIBUTING.zh-CN.md)). Report security problems as described in
[SECURITY.md](SECURITY.md), not in a public issue.

## License

Software (firmware and Companion) is licensed under the
[Apache License 2.0](LICENSE). Hardware design files in `hardware/` are also
published on OSHWHub and keep the licence stated on that project page. Fonts
and other third-party material keep their own licences; see [NOTICE](NOTICE)
and the licence texts in `firmware/app/third_party/fonts/`.
