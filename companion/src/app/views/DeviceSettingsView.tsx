/* Device settings: what belongs to the Ki-board itself. Volume and screen
 * brightness on one row, then the keyboard's own settings page (voice mode,
 * key lights, screen font, animation). Link and network setup live under
 * Connection; rarely used actions under Advanced settings. */
import { useEffect, useState } from "react";
import { Card } from "../../ui/primitives";
import { KeyboardSettingsCard } from "../../ui/BoardStatusCards";
import { deviceInfoRows } from "../deviceInfo";
import type { Model } from "../useCompanion";

export function DeviceSettingsView({ m }: { m: Model }) {
  const t = m.t;
  const status = m.status;
  const ready = status.device_connected && status.device_settings_synced && !m.deviceSettingsBusy;

  const stepper = (key: "volume" | "brightness", label: string, minimum: number) => (
    <div className="kc-stepper" role="group" aria-label={label}>
      <span>{label}</span>
      <button
        type="button"
        className="kc-btn ghost"
        aria-label={`${label} −`}
        disabled={!ready || m.deviceSettings[key] <= minimum}
        onClick={() => m.runAction(() => m.adjustDeviceSetting(key, -10))}
      >
        −
      </button>
      <strong>{m.deviceSettings[key]}%</strong>
      <button
        type="button"
        className="kc-btn ghost"
        aria-label={`${label} +`}
        disabled={!ready || m.deviceSettings[key] >= 100}
        onClick={() => m.runAction(() => m.adjustDeviceSetting(key, 10))}
      >
        +
      </button>
    </div>
  );

  return (
    <div className="kc-stack">
      <Card title={t("conn.settings.title")} info="help.conn.settings">
        <div className="kc-stepper-row">
          {stepper("volume", t("conn.settings.volume"), 0)}
          {stepper("brightness", t("conn.settings.brightness"), 10)}
        </div>
        <div className="kc-muted kc-note">
          {!status.device_connected ? t("conn.sync.offline") : status.device_settings_synced ? t("conn.sync.synced") : t("conn.sync.waiting")}
        </div>
      </Card>
      <KeyboardSettingsCard status={m.board} />
      <DeviceInfoCard m={m} />
      {m.uiError && <div className="error-banner">{m.uiError}</div>}
    </div>
  );
}

/* Read-only device info. "--" = the keyboard has not reported it (some items
 * arrive over USB only, others need a newer firmware). */
function DeviceInfoCard({ m }: { m: Model }) {
  const t = m.t;
  const [copied, setCopied] = useState("");
  const rows = deviceInfoRows(
    {
      connected: m.status.device_connected,
      transport: m.status.transport,
      handshakeMissing: !!m.status.handshake_missing,
      firmware: m.status.firmware_version,
      info: m.board.deviceInfo,
      system: m.board.systemInfo,
      keyboard: m.board.keyboard,
      license: m.board.license,
    },
    t,
  );
  useEffect(() => {
    if (!copied) return;
    const timer = window.setTimeout(() => setCopied(""), 1500);
    return () => window.clearTimeout(timer);
  }, [copied]);

  return (
    <Card title={t("info.title")} info="help.info">
      <dl className="kc-info">
        {rows.map((row) => (
          <div className="kc-info-row" key={row.id}>
            <dt>{row.label}</dt>
            <dd className={row.copy === null ? "kc-muted" : ""}>{row.value}</dd>
            <button
              type="button"
              className="kc-btn ghost kc-info-copy"
              disabled={row.copy === null}
              aria-label={`${t("info.copy")}: ${row.label}`}
              onClick={() => {
                const text = row.copy;
                if (text !== null) m.runAction(() => navigator.clipboard.writeText(text).then(() => setCopied(row.id)));
              }}
            >
              {copied === row.id ? t("info.copied") : t("info.copy")}
            </button>
          </div>
        ))}
      </dl>
    </Card>
  );
}
