# FP-6 ble-hid-fallback — T1 spike (result: **NO-GO**, evidence artifact)

> ⚠️ **This probe does NOT build.** It is kept as the *evidence* for the FP-6
> GO/NO-GO decision, not as a runnable firmware. Do not flash it.
> Full reasoning: `.kiro/specs/feature-probes-ble-hid-fallback/design.md` §D10
> and `tasks.md` T1.

## What this is

A T1 spike to answer: *can a standard BLE HID keyboard (HOGP / HID-over-GATT) be
brought up on T5AI-Core with TuyaOpen, within the "app source only, don't patch
the vendored SDK" boundary?*

`src/tuya_app_main.c` is a complete, API-correct compliant-HOGP skeleton written
against the **native NimBLE** host: HID service `0x1812` (Report Map `0x2A4B`,
HID Info `0x2A4A`, HID Control Point `0x2A4C`, Protocol Mode `0x2A4E`, Input
Report `0x2A4D` carrying a **Report Reference descriptor `0x2908`** + CCCD),
DIS `0x180A`, Battery `0x180F`, Security Manager pairing/bonding via
`ble_hs_cfg.sm_*` + `ble_store_*`, keyboard-appearance advertising
(`0x03C1` + service `0x1812`), and key sends via `ble_gatts_notify_custom()`.

Every referenced symbol was verified to exist in the SDK headers (see the D10
API table).

## Why it is NO-GO

`tos.py build` fails **at the header layer**, before linking, on the app source:

1. **`LIST_HEAD` macro clash (-Werror):** NimBLE's `tuya_ble_linkqueue.h:266`
   defines a 2-arg `LIST_HEAD(name, type)`; TuyaOS's `tuya_list.h:34` defines a
   1-arg `LIST_HEAD(name)`. Neither is `#ifndef`-guarded, and they are
   incompatible. `ble_att.h:30` includes `tuya_ble_linkqueue.h`, and `ble_gatt.h`
   includes `ble_att.h` — so *any* `ble_*.h` collides with the mandatory
   `tal_api.h` (which pulls `tuya_list.h`).
2. **Host-private header missing:** `ble_hs.h → hci_common.h → tuya_ble.h`
   includes `tuya_ble_mempool.h`, which lives ONLY under `nimble/host/`
   (`LIB_PRIVATE_INC`, not exported to apps) → `fatal error: No such file`.

Root cause: the native NimBLE host is **not an app-facing API** in this SDK.
Every `.c` in the whole tree that includes `ble_gap.h`/`ble_hs.h` is inside
`nimble/host/`, `nimble/modules/`, or `tkl_bluetooth.c` — all compiled with the
private include path. No app/example includes them.

## To turn this GO (both require going beyond a minimal probe)

- **(A)** Implement HID inside the SDK: add e.g. `ble_svc_hid.c` under
  `src/tal_bluetooth/nimble/` so it compiles with the host-private include path
  (i.e. fork/patch the vendored SDK).
- **(B)** Upstream provides an app-level HOGP port.

Both need dedicated scheduling and a decision to patch/fork the SDK.
