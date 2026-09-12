# FP-6 ble-hid-fallback — compile+link spike (result: **GO**, builds clean)

> ✅ **This probe now BUILDS (compile AND link) with zero SDK edits.**
> `tos.py build` => `BUILD SUCCESS`. Hardware pairing/typing is the user's later
> test (spec T3/T5). Full reasoning:
> `.kiro/specs/feature-probes-ble-hid-fallback/design.md` §D11 (and §D10 for the
> earlier single-TU failure it overturns), `tasks.md` T1b.

## What this is

A compile+link spike answering: *can a standard BLE HID keyboard (HOGP /
HID-over-GATT) be built on T5AI-Core with TuyaOpen, within the "app source only,
don't patch the vendored SDK" boundary?* Answer: **yes**, via an isolation
architecture.

## Isolation architecture (the crux)

The D10 spike failed only because it put `tal_api.h` and `ble_*.h` in **one**
translation unit, where the two incompatible `LIST_HEAD` macros collide. The fix
is to keep the two header worlds in **separate** TUs:

| File | Includes | Role |
|---|---|---|
| `src/ak_ble_hid.c` | **only** nimble headers (`ble_*.h`, `tuya_ble_mbuf.h`) — never a TAL header | all native HOGP: GATT DB, Security Manager, advertising, Input Report notify |
| `include/ak_ble_hid.h` | **plain C** (`stdint.h`/`stdbool.h`) — no nimble/TAL types | the bridge API (`ak_ble_hid_init/ready/connected/send_char/send_key/send_text`) |
| `src/tuya_app_main.c` | **only** `tal_api.h` + `ak_ble_hid.h` — never a nimble header | TAL host bring-up, calls the bridge on `TAL_BLE_STACK_INIT`, status LCD |

Because neither TU sees both header worlds, the D10 blockers vanish:

1. **`LIST_HEAD` macro clash** — gone: `ak_ble_hid.c` only sees the 2-arg
   `LIST_HEAD` from `tuya_ble_linkqueue.h`; `tuya_app_main.c` only sees the 1-arg
   one from `tuya_list.h`. (Verified: no `nimble/include` header pulls
   `tuya_list.h`.)
2. **Host-private `tuya_ble_mempool.h` missing** — solved at the **probe
   CMakeLists** level by adding `${OPEN_SDK_ROOT}/src/tal_bluetooth/nimble/host`
   (+ `.../include`) to `ak_ble_hid.c`'s include path. **No SDK file is edited**;
   `nimble/host` is merely referenced by this probe's compile command.

## What it implements (all compiled + linked)

HID service `0x1812` (Report Map `0x2A4B`, HID Info `0x2A4A`, HID Control Point
`0x2A4C`, Protocol Mode `0x2A4E`, Input Report `0x2A4D` carrying a **Report
Reference descriptor `0x2908`**, NOTIFY auto-adds CCCD), DIS `0x180A`, Battery
`0x180F`, Security Manager (Just Works / bonding / LE Secure Connections) via
`tuya_ble_hs_cfg.sm_*` + `ble_store_*`, keyboard-appearance advertising
(`0x03C1` + service `0x1812`) via native `ble_gap_adv_set_fields()`/`_start()`,
and key sends via `ble_gattc_notify_custom()`.

Non-stub proof: `nm ak_ble_hid.c.o` shows every native symbol as `U`
(undefined → resolved from `libtal_bluetooth.a` at link).

## Two API corrections vs the D10 table (found at compile time)

- Notify uses **`ble_gattc_notify_custom(conn, att_handle, om)`** — this SDK's
  public headers have no `ble_gatts_notify_custom`.
- The SM global is **`tuya_ble_hs_cfg`** (`ble_hs.h:286`) — there is no
  `ble_hs_cfg` alias in this SDK. Field names are unchanged.

## Build

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd firmware/feature-probes/ble_hid_fallback   # (repo-relative)
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py clean -f && tos.py build   # => BUILD SUCCESS; dist/ble_hid_fallback_0.1.0/*.bin
```

## Still unverified (hardware)

Advertising actually starting, Windows enumerating the HID device, Just Works
pairing + reconnect-without-repair (design D7 / task T3). Flash on the extension
board (COM6) and test:

```
.\flash.ps1 -Port COM6 -App "firmware/feature-probes/ble_hid_fallback" -Monitor
```

`.build/` and `dist/` are gitignored (`firmware/**/.build/`, `firmware/**/dist/`).
Main-line FP-2/3/4/5 builds are unaffected — the extra include dirs apply only to
this probe's target.
