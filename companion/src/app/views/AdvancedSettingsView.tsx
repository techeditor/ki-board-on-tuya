/* Advanced settings: the rarely used actions. Tuya UUID / AuthKey status, Bluetooth pairing,
 * unbinding the device, the Tuya PID / UUID / AuthKey and firmware updates. Most of
 * them need USB, and several only work while the pairing window is open on
 * the keyboard. */
import { useState } from "react";

import { Card, StateBadge } from "../../ui/primitives";
import { BlePairingCard, CredentialsCard } from "../../ui/BoardStatusCards";
import { FirmwareUpdateCard } from "../../ui/FirmwareUpdateCard";
import { showUnbind } from "../deviceInfo";
import type { Model } from "../useCompanion";

export function AdvancedSettingsView({ m }: { m: Model }) {
  const t = m.t;
  const status = m.status;
  const [confirmUnbind, setConfirmUnbind] = useState(false);
  // Like the keyboard: no "Unbind device" until the keyboard is activated on Tuya (nothing to unbind;
  // Pairing window on the Connection page does the pairing). Unknown state keeps it.
  const unbindShown = showUnbind(status.device_connected, m.board.deviceInfo);
  const confirmingUnbind = confirmUnbind && unbindShown;
  const provLocked = !m.usbOk || !m.provWindowOpen;

  return (
    <div className="kc-stack">
      <CredentialsCard status={m.board} />
      <BlePairingCard status={m.board} usbConnected={m.usbOk} />

      <Card title={t("conn.lic.title")} info="help.conn.lic">
        <div className="kc-field" style={{ marginTop: 0 }}>
          <span>{t("conn.pid.label")}</span>
          {m.deviceSettings.pid === "present" || m.deviceSettings.pid === "pending" ? (
            <StateBadge tone="success">{m.deviceSettings.pid === "pending" ? t("conn.pid.pending") : t("conn.pid.set")}</StateBadge>
          ) : (
            <>
              <input
                placeholder={t("conn.pid.placeholder")}
                value={m.pidInput}
                maxLength={16}
                disabled={!m.usbOk || m.pidBusy}
                onChange={(e) => m.setPidInput(e.target.value.toLowerCase())}
                style={{ flex: 1 }}
              />
              <button
                type="button"
                className="kc-btn"
                disabled={!m.usbOk || m.pidBusy || m.pidInput.trim().length !== 16}
                onClick={() => m.runAction(m.sendPid)}
              >
                {t("conn.pid.write")}
              </button>
            </>
          )}
        </div>
        {m.pidStatus && <div className="kc-muted kc-note">PID · {m.pidStatus}</div>}
        <div className="kc-field">
          <input placeholder={t("conn.lic.uuid")} value={m.licUuid} disabled={provLocked || m.licBusy} onChange={(e) => m.setLicUuid(e.target.value)} style={{ flex: 1 }} />
        </div>
        <div className="kc-field">
          <input type="password" placeholder={t("conn.lic.authkey")} value={m.licAuthkey} disabled={provLocked || m.licBusy} onChange={(e) => m.setLicAuthkey(e.target.value)} style={{ flex: 1 }} />
        </div>
        <div className="kc-field">
          <button type="button" className="kc-btn" disabled={provLocked || m.licBusy} onClick={() => m.runAction(m.sendLicense)}>{t("conn.lic.write")}</button>
          <button type="button" className="kc-btn ghost" disabled={provLocked || m.licBusy} onClick={() => m.runAction(m.readLicense)}>{t("conn.lic.read")}</button>
        </div>
        <div className="kc-field"><span>{t("conn.lic.state")}</span><strong>{m.licStatus || t("conn.lic.notWritten")}</strong></div>
        {m.licInfo && (
          <div className="kc-muted kc-note">
            {t("conn.lic.source")} <code>{m.licInfo.source}</code> · UUID <code>{m.licInfo.uuid || "--"}</code> · AuthKey{" "}
            <code>{m.licInfo.authkey_tail ? `****${m.licInfo.authkey_tail}` : "--"}</code>
          </div>
        )}
      </Card>

      {unbindShown && (
        <Card title={t("conn.unbind.title")} info="help.conn.unbind">
          <div className="kc-field" style={{ marginTop: 0 }}>
            <span>{t("conn.pairing.status")}</span>
            <strong>{m.pairingStatus || t("common.idle")}</strong>
          </div>
          {!confirmingUnbind ? (
            <div className="kc-field">
              <button type="button" className="kc-btn ghost" disabled={!m.usbOk || m.pairingBusy} onClick={() => setConfirmUnbind(true)}>
                {t("conn.unbind.button")}
              </button>
            </div>
          ) : (
            <div className="kc-field" style={{ alignItems: "flex-start" }}>
              <span>{t("conn.unbind.confirm")}</span>
              <button
                type="button"
                className="kc-btn"
                onClick={() => {
                  setConfirmUnbind(false);
                  m.runAction(m.enterPairing);
                }}
              >
                {t("conn.unbind.yes")}
              </button>
              <button type="button" className="kc-btn ghost" onClick={() => setConfirmUnbind(false)}>{t("common.cancel")}</button>
            </div>
          )}
          {!m.usbOk && <div className="kc-muted kc-note">{t("conn.error.usbPair")}</div>}
        </Card>
      )}

      <Card title={t("ota.title")}>
        <FirmwareUpdateCard usb={m.usbOk} firmware={status.firmware_version} />
      </Card>

      {m.uiError && <div className="error-banner">{m.uiError}</div>}
    </div>
  );
}
