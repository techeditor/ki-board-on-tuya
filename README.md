# Ki-board: Agentic Keyboard for Kiro (Tuya T5AI-Core)

A small macro keyboard built on the **Tuya T5AI-Core** board for working with
Kiro. It sends voice input into Kiro and shows agent status on its own screen.

*(中文版见 [README.zh-CN.md](README.zh-CN.md))*

This repository holds the source of release **v0.3.63**: firmware 0.3.63,
Companion 0.3.4, the wire protocol and the hardware design files.

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

## Repository layout

```text
.
├── firmware/app/        # product firmware (TuyaOpen, C)
├── firmware/assets/     # icon sources for the firmware bitmaps
├── companion/           # desktop Companion app (Tauri: Rust + React/TS)
├── shared/protocol/     # authoritative wire-protocol spec
├── docs/                # protocol guide, OTA, key gestures, versioning
└── hardware/            # pin map and design exports
```

## Firmware

Needs a [TuyaOpen](https://github.com/tuya/TuyaOpen) checkout.

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE.config
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
cd src-tauri && cargo test   # protocol / transport unit tests
```

## Documentation

| Doc | Content |
|-----|---------|
| [shared/protocol/protocol.md](shared/protocol/protocol.md) | Authoritative protocol spec |
| [docs/protocol.md](docs/protocol.md) | Protocol reader's guide |
| [docs/ota.md](docs/ota.md) | Local OTA over USB: package format, keys, release |
| [docs/firmware-key-gesture-events-spec.md](docs/firmware-key-gesture-events-spec.md) | Key gesture events |
| [docs/versioning.md](docs/versioning.md) | Version numbering |

## License

Software (firmware, Companion, tools) is licensed under the
[Apache License 2.0](LICENSE). Hardware design files in `hardware/` are also
published on OSHWHub and keep the licence stated on that project page. Fonts
and other third-party material keep their own licences; see [NOTICE](NOTICE)
and `firmware/app/third_party/fonts/THIRD_PARTY.md`.
