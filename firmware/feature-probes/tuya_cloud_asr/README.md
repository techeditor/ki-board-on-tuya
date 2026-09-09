# FP-3 · tuya-cloud-asr

Validates MVP requirement (3): **board mic → Tuya cloud ASR → recognized text**.
On each recognized utterance the text is emitted on **UART0** as protocol-v1
JSONL, so the pipeline is observable on serial (and later rides BLE GATT to the
companion → Kiro Crew in FP-7).

## Provenance (D8 route)

This probe is **derived from** `TuyaOpen/apps/tuya.ai/your_chat_bot` (Apache-2.0),
per the spec's D8 fallback: copied as the base, then trimmed to "get ASR text".
Removed the rich-display UI (`src/display2/**`), battery, and all non-`TUYA_T5AI_CORE`
board configs. Vendored SDK files keep their original license headers.

The only project-specific additions/edits:
- `include/ak_tuya_authkey.h.example` — tracked license template.
- `include/ak_tuya_authkey.h` — **gitignored** real license (copy from the
  template, fill from `docs.local/tuya_keys.txt`). Never committed.
- `include/tuya_config.h` — wires the SDK license macros to `AK_TUYA_*`.
- `Kconfig` — `select ENABLE_AI_COMPONENTS` (pulls the AI stack) + PID.
- `src/app_chat_bot.c` — added `__fp3_emit_asr()` + `AI_USER_EVT_ASR_OK/ERROR/EMPTY`
  cases that write JSONL to UART0 (via `firmware/app/common/ak_uart`), and
  `ak_uart_init()` in `app_chat_bot_init()`.

## Behavior

- Mode: `AI_CHAT_MODE_HOLD` (push-to-talk) — matches the K4-PTT interaction
  design (spec asked ONESHOT; HOLD/PTT chosen as it fits the product; switchable
  in `app_chat_bot_init`).
- On `AI_USER_EVT_ASR_OK`: `{"v":1,"type":"asr","payload":{"text":"<escaped>","ms":<timeindex>}}`
- On ASR error/empty: `{"v":1,"type":"asr","payload":{"error":"asr_error|asr_empty"}}`

UART0 = P10/P11 @ 115200 8N1 (expansion board → COM6). Tuya debug log stays on
UART1 @ 460800.

## License setup + build

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/tuya_cloud_asr
cp include/ak_tuya_authkey.h.example include/ak_tuya_authkey.h   # then fill real UUID/AuthKey/PID
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py clean -f && tos.py build            # AP ~1.77MB / 3.5MB
```

Flash (expansion board = COM6):
```powershell
.\flash.ps1 -Port COM6 -App "firmware/feature-probes/tuya_cloud_asr" -Monitor
```

## Real-hardware acceptance (T6, pending)

Needs the v1.2 board + 2.4GHz Wi-Fi + a valid license. First boot requires Wi-Fi
provisioning (BLE/AP netcfg). Then hold the voice key, speak, and confirm an
`asr` text frame appears on COM6 matching what was said.
