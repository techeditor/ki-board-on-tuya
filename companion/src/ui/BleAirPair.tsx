/* Over-the-air Bluetooth pairing (protocol.md "蓝牙直接配对"): no USB and no
 * Tuya cloud. The user opens Settings → BLE pairing on the keyboard, picks the
 * scanned keyboard here and starts; both sides then show the same 6-digit
 * code. Confirm here only when the codes match, and press K6 on the keyboard.
 * The key itself stays in the backend (ble_pairing); the UI sees its id. */
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { useI18n } from "../i18n";
import { StateBadge } from "./primitives";

type AirPairEvent = { state: "code" | "done" | "failed"; code?: string; key_id?: string; reason?: string };
type Phase = { kind: "idle" } | { kind: "connecting" } | { kind: "code"; code: string; answered: boolean }
  | { kind: "done"; keyId: string } | { kind: "failed"; reason: string };

export function BleAirPair({ deviceId }: { deviceId: string }) {
  const { t } = useI18n();
  const [phase, setPhase] = useState<Phase>({ kind: "idle" });

  useEffect(() => {
    const off = listen<AirPairEvent>("ble-air-pair", (event) => {
      const e = event.payload;
      if (e.state === "code") setPhase({ kind: "code", code: e.code ?? "", answered: false });
      else if (e.state === "done") setPhase({ kind: "done", keyId: e.key_id ?? "" });
      else setPhase({ kind: "failed", reason: e.reason ?? "" });
    });
    return () => {
      void off.then((f) => f());
    };
  }, []);

  const start = () => {
    setPhase({ kind: "connecting" });
    invoke("ble_pair_air", { deviceId }).catch((e) => setPhase({ kind: "failed", reason: String(e) }));
  };
  const answer = (confirm: boolean) => {
    if (phase.kind === "code") setPhase({ ...phase, answered: true });
    invoke("ble_pair_air_answer", { confirm }).catch(() => undefined);
  };
  const busy = phase.kind === "connecting" || phase.kind === "code";

  return (
    <div className="kc-stack" style={{ gap: 6 }}>
      <div className="kc-field" style={{ marginTop: 0 }}>
        <button type="button" className="kc-btn ghost" disabled={busy || deviceId === ""} onClick={start}>
          {t("conn.airPair.start")}
        </button>
        <span className="kc-muted kc-note">{t("conn.airPair.how")}</span>
      </div>
      {phase.kind === "connecting" && <div className="kc-muted kc-note">{t("conn.airPair.connecting")}</div>}
      {phase.kind === "code" && (
        <div className="kc-field" role="alert">
          <span>{t("conn.airPair.code")}</span>
          <strong className="kc-paircode">{phase.code}</strong>
          <button type="button" className="kc-btn" disabled={phase.answered} onClick={() => answer(true)}>
            {t("conn.airPair.match")}
          </button>
          <button type="button" className="kc-btn ghost" disabled={phase.answered} onClick={() => answer(false)}>
            {t("conn.airPair.noMatch")}
          </button>
          <span className="kc-muted kc-note">
            {phase.answered ? t("conn.airPair.waitK6") : t("conn.airPair.compare")}
          </span>
        </div>
      )}
      {phase.kind === "done" && <StateBadge tone="success">{t("conn.airPair.done", { id: phase.keyId })}</StateBadge>}
      {phase.kind === "failed" && <StateBadge tone="warning">{t("conn.airPair.failed", { reason: phase.reason })}</StateBadge>}
    </div>
  );
}
