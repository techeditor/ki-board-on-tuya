# Agentic Keyboard for Kiro (Tuya T5AI-Core)

A portable AI macro-keyboard built on the **Tuya T5AI-Core** board, designed to
improve the Kiro CLI / IDE development experience. Its differentiators are
**voice input straight into Kiro** and **on-screen Agent status**.

> Status: **Phase 0 complete; Phase 0.5 hardware bring-up in progress**. The
> expansion baseboard v1.0 design and review are complete. Six standalone
> bring-up applications build successfully for the project IO and await
> sequential testing on the physical board. Product firmware and the companion
> app remain skeletons.

*(中文版见 [README.zh-CN.md](README.zh-CN.md))*

## Hardware

> Versioning scheme: see [docs/versioning.md](docs/versioning.md). Current iteration **v0.3** (major refactor, in design).

| Part | Model (v0.3) |
|------|-------|
| MCU | T5AI-Core (T5-E1 module, Cortex-M33F @ 480 MHz, Wi-Fi6 + BLE 5.4) |
| Display | T280C7-C12-26 LCD (ST7789P3, 2.8" IPS, 320×240, SPI, 12P 0.5mm bottom-contact FPC) |
| Keys | 6× mechanical keys K1–K6, hot-swap sockets (Kailh MX-compatible) |
| Navigation | EC11 rotary encoder (A/B + push) |
| Indicators | 7× WS2812 RGB (one under each of K1–K6, LED7 under/near EC11) |
| Audio | onboard mic / speaker |
| Baseboard | **passive**: keys + display + LEDs only; USB/power/charging all handled by the dev board |

> v0.2 (previous, PCB silkscreen v1.2.0) used 5× brown switches + WS-1002 5-way joystick + CL28CK230-18A (18P) + on-baseboard USB/CH340C.

## Architecture

- **Firmware (TuyaOpen C)**: input scan + debounce → event & state machine →
  LVGL @ ST7789 UI + pluggable transport (UART / BLE GATT / BLE HID).
- **Companion (Tauri: Rust + React/TS)**: transport backend + protocol parser →
  event routing/mapping → Win32 SendInput injection / clipboard paste / Windows
  system ASR → inject into the focused Kiro window; Kiro CLI hook (JSONL) →
  reflect Agent status back to the board.

## Roadmap (phased, each independently demoable)

1. **Phase 0** — Scaffolding & hardware definition (repo skeleton, pin map).
2. **Phase 1** — Wired MVP: serial + Tauri companion + Windows system ASR.
3. **Phase 2** — Wireless: BLE GATT + on-board wake word + online ASR.
4. **Phase 3** — BLE HID direct connect + optional on-board local ASR.

Full task breakdown: [docs/implementation-plan.md](docs/implementation-plan.md).

## Repository layout

```text
.
├── firmware/            # TuyaOpen product skeleton + standalone bring-up apps
├── companion/           # Tauri (Rust + React/TS) companion app
├── hardware/
│   └── pinmap/          # GPIO assignment + netlist (Task 2)
├── enclosure/           # 3D-printed enclosure design (independent sub-project)
├── shared/protocol/     # Authoritative wire-protocol spec
├── docs/                # Design / spec docs (Chinese-first)
└── ref_projects/        # External references (git-ignored, not part of this repo)
    ├── ki-board/
    └── signal_light_demo/
```

## Build (skeleton)

Prerequisites: Node 22+, Rust 1.96+.

```bash
cd companion
npm ci
npm run tauri dev        # launch the skeleton window
cd src-tauri && cargo test   # run protocol/transport unit tests
```

## Documentation

| Doc | Content |
|-----|---------|
| [prd.md](docs/prd.md) | **Product requirements (for circuit review & code dev)** |
| [implementation-plan.md](docs/implementation-plan.md) | Requirements, decisions, 16-task plan |
| [protocol.md](docs/protocol.md) | Protocol reader's guide (points to the source) |
| [shared/protocol/protocol.md](shared/protocol/protocol.md) | Authoritative protocol spec |
| [hardware/pinmap/](hardware/pinmap/) | Pin map, netlist, BOM, conflict checker |

## Conventions

- Interaction & design docs: Chinese-first. README: bilingual (English primary).
- Code comments: English. Commit messages: English.
- `ref_projects/` contains **references only** and is excluded from version control.

## License

For learning and personal use. Third-party libraries retain their own licenses.
