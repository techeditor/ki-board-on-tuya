/* Connection (design §1): how the PC reaches the keyboard and how the keyboard
 * gets online: transport, the pairing window, Wi-Fi with SmartLife, saved
 * networks. Keyboard settings are under Device settings, rarely used actions
 * under Advanced settings. Explanations sit behind InfoTip (R5). */
import { Card, StateBadge } from "../../ui/primitives";
import { PairingWindowCard, SavedWifiCard } from "../../ui/BoardStatusCards";
import { BleAirPair } from "../../ui/BleAirPair";
import { ACTIVATION_STEPS, activationLabelKey } from "../companionTypes";
import type { Model } from "../useCompanion";

export function ConnectionView({ m }: { m: Model }) {
  const t = m.t;
  const status = m.status;
  const provLocked = !m.usbOk || !m.provWindowOpen;

  return (
    <div className="kc-stack">
      <Card title={t("conn.transport.title")} info="help.conn.transport">
        <div className="kc-keymap-head">
          {(["auto", "usb", "ble"] as const).map((mode) => (
            <button
              key={mode}
              type="button"
              className={`kc-seg ${m.connPrefs.transport === mode ? "on" : ""}`}
              onClick={() => m.runAction(() => m.updateConnPrefs({ transport: mode }))}
            >
              {t(`conn.mode.${mode}`)}
            </button>
          ))}
        </div>
        {m.connPrefs.transport !== "ble" && (
          <div className="kc-field" style={{ marginTop: 0 }}>
            <span>{t("conn.transport.port")}</span>
            <select value={m.connPrefs.port} onChange={(e) => m.runAction(() => m.updateConnPrefs({ port: e.target.value }))}>
              <option value="">{t("conn.transport.auto")}</option>
              {m.boardPorts.map((port) => (
                <option key={port.name} value={port.name}>{port.name} · {port.kind}</option>
              ))}
            </select>
          </div>
        )}
        {m.connPrefs.transport !== "usb" && (
          <div className="kc-field" style={{ marginTop: 0 }}>
            <span>{t("conn.transport.device")}</span>
            <select value={m.connPrefs.ble_device} onChange={(e) => m.runAction(() => m.updateConnPrefs({ ble_device: e.target.value }))}>
              {m.bleDevices.length === 0 && m.connPrefs.ble_device === "" && <option value="">{t("conn.transport.noDevices")}</option>}
              {m.connPrefs.ble_device !== "" && !m.bleDevices.some((d) => d.id === m.connPrefs.ble_device) && (
                <option value={m.connPrefs.ble_device}>{m.connPrefs.ble_device}</option>
              )}
              {m.bleDevices.map((dev) => (
                <option key={dev.id} value={dev.id}>{dev.name || "Ki-board"} · {dev.id}</option>
              ))}
            </select>
            <button type="button" className="kc-btn ghost" disabled={m.bleScanning} onClick={() => m.runAction(m.scanBle)}>
              {m.bleScanning ? t("conn.transport.scanning") : t("conn.transport.scan")}
            </button>
          </div>
        )}
        {m.connPrefs.transport !== "usb" && <BleAirPair deviceId={m.connPrefs.ble_device} />}
        <div className="kc-field">
          <label className="kc-check">
            <input
              type="checkbox"
              checked={m.connPrefs.auto_connect}
              onChange={(e) => m.runAction(() => m.updateConnPrefs({ auto_connect: e.target.checked }))}
            />
            {t("conn.autoConnect")}
          </label>
          <span className="kc-muted kc-note">{t("conn.autoConnect.hint")}</span>
        </div>
        <div className="kc-field">
          <button type="button" className="kc-btn" onClick={() => m.runAction(m.connect)}>{t("common.connect")}</button>
          <button type="button" className="kc-btn ghost" onClick={() => m.runAction(m.disconnect)}>{t("common.disconnect")}</button>
          <button type="button" className="kc-btn ghost" onClick={() => m.runAction(m.refreshRuntime)}>{t("common.refresh")}</button>
        </div>
        <div className="kc-field" style={{ marginTop: 12 }}>
          <Metric label={t("conn.metric.link")} value={status.device_connected ? status.transport.toUpperCase() : t("status.offline")} />
          <Metric label={t("conn.metric.fw")} value={status.firmware_version || "--"} />
          <Metric label={t("conn.metric.voice")} value={m.voiceLabel} />
        </div>
        {status.device_connected && status.handshake_missing && (
          <div className="kc-pending" role="status" style={{ marginTop: 12 }}>
            <b>{t("status.noHandshake")}</b>
          </div>
        )}
      </Card>

      <PairingWindowCard status={m.board} usbConnected={m.usbOk} />
      <Card title={t("conn.wifi.setup.title")} info="help.conn.wifi.setup">
        {!m.usbOk && <div className="kc-muted kc-note">{t("conn.usbRequired")}</div>}
        <div className="kc-field" style={{ marginTop: 6 }}>
          <input
            placeholder={t("conn.wifi.ssid")}
            value={m.wifiSsid}
            disabled={!m.usbOk || m.wifiBusy}
            onChange={(e) => m.setWifiSsid(e.target.value)}
            style={{ flex: 1 }}
          />
        </div>
        <div className="kc-field">
          <input
            type="password"
            placeholder={t("conn.wifi.pass")}
            value={m.wifiPass}
            disabled={!m.usbOk || m.wifiBusy}
            onChange={(e) => m.setWifiPass(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter" && m.usbOk && !m.wifiBusy) m.runAction(m.sendWifi);
            }}
            style={{ flex: 1 }}
          />
          <button type="button" className="kc-btn" disabled={provLocked || m.wifiBusy} onClick={() => m.runAction(m.sendWifi)}>
            {t("conn.wifi.send")}
          </button>
        </div>
        <div className="kc-field"><span>{t("conn.wifi.state")}</span><strong>{m.wifiStatus || t("conn.wifi.notSent")}</strong></div>

        <div className="kc-field" style={{ marginTop: 12 }}>
          <span>{t("act.title")}</span>
          <StateBadge tone={m.activationStatus.state === "cloud_ready" ? "success" : "neutral"}>
            {t(activationLabelKey(m.activationStatus.state))}
          </StateBadge>
          <button type="button" className="kc-btn ghost" onClick={() => m.setActivationExpanded(!m.activationExpanded)}>
            {m.activationExpanded ? t("common.hide") : t("common.show")}
          </button>
        </div>
        {m.activationExpanded && (
          <div className="kc-stack" style={{ marginTop: 8 }}>
            <div className="kc-field">
              {ACTIVATION_STEPS.map((step, index) => {
                const current = m.activationStepIndex(m.activationStatus.state);
                return (
                  <StateBadge key={step} tone={index < current ? "success" : index === current ? "accent" : "neutral"}>
                    {index + 1} {t(`act.step.${step}`)}
                  </StateBadge>
                );
              })}
            </div>
            {m.activationStatus.state === "scan_qr" && (
              <div className="kc-field" style={{ alignItems: "flex-start" }}>
                {m.activationQrDrawing ? (
                  <svg
                    role="img"
                    aria-label={t("act.qrLabel")}
                    width={180}
                    height={180}
                    viewBox={`-1 -1 ${m.activationQrDrawing.size + 2} ${m.activationQrDrawing.size + 2}`}
                    shapeRendering="crispEdges"
                  >
                    <rect x="-1" y="-1" width={m.activationQrDrawing.size + 2} height={m.activationQrDrawing.size + 2} fill="#fff" />
                    <path d={m.activationQrDrawing.path} fill="#000" />
                  </svg>
                ) : (
                  <span className="kc-muted">{m.activationQrError || t("act.generating")}</span>
                )}
                <span className="kc-muted">{t("act.scanHint")}</span>
              </div>
            )}
            <div className="kc-muted kc-note">{t(`act.msg.${m.activationStatus.state}`)}</div>
          </div>
        )}
      </Card>
      <SavedWifiCard status={m.board} usbConnected={m.usbOk} />

      {m.uiError && <div className="error-banner">{m.uiError}</div>}
      {(status.last_error || "") !== "" && <div className="error-banner">{status.last_error}</div>}
    </div>
  );
}

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="kc-metric">
      <span>{label}</span>
      <b>{value}</b>
    </div>
  );
}
