/* Connection-view cards: the FR-02 keyboard settings (normal items, USB or
 * BLE), the pairing window, credentials, BLE pairing and voice mode
 * (redesign tasks T4.1; design §1 "Connection"). Theme tokens, i18n and
 * InfoTip only — explanations live in help.* popovers (R5). Sensitive items
 * (pairing window, BLE unpair, Wi-Fi, License, reset) are USB only and the
 * keyboard asks for K6 before acting; over BLE their buttons stay disabled
 * and the cards show status only. */
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { useI18n } from "../i18n";
import { Card, StateBadge } from "./primitives";
import { provReasonKey, provRemainingS, type BoardStatus } from "./boardStatus";

function mmss(total: number): string {
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

/** Pairing window: open with countdown, or closed with how-to behind (i). */
export function PairingWindowCard({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  const { t } = useI18n();
  const [error, setError] = useState("");
  const left = provRemainingS(status);
  const open = usbConnected && left > 0;
  const rejected = status.rejection.seq > 0 && status.rejection.reason !== "confirm_on_device";
  const ask = () => {
    setError("");
    invoke("keyboard_open_pairing").catch((e) => setError(String(e)));
  };
  return (
    <Card title={t("conn.pairing.title")} info="help.conn.pairing">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <span>{t("conn.pairing.state")}</span>
        {open ? (
          <StateBadge tone="accent">{t("conn.pairing.open", { time: mmss(left) })}</StateBadge>
        ) : (
          <StateBadge tone="neutral">{t("conn.pairing.closed")}</StateBadge>
        )}
      </div>
      {!open && <div className="kc-muted kc-note">{t("conn.pairing.how")}</div>}
      {!open && (
        <div className="kc-field">
          <button type="button" className="kc-btn" disabled={!usbConnected} onClick={ask}>
            {t("conn.pairing.ask")}
          </button>
          <span className="kc-muted">{usbConnected ? t("conn.pairing.askHint") : t("kb.usbOnly")}</span>
        </div>
      )}
      {error && <StateBadge tone="warning">{error}</StateBadge>}
      {rejected && (
        <div className="kc-field">
          <StateBadge tone="warning">{t("conn.pairing.refused", { cmd: status.rejection.command })}</StateBadge>
          <span>{t(provReasonKey(status.rejection.reason))}</span>
        </div>
      )}
    </Card>
  );
}

/** License presence only. UUID / AuthKey values never reach this view. */
export function CredentialsCard({ status }: { status: BoardStatus }) {
  const { t } = useI18n();
  const tone = status.license === "present" ? "success" : status.license === "missing" ? "warning" : "neutral";
  const label =
    status.license === "present"
      ? t("conn.cred.present")
      : status.license === "missing"
        ? t("conn.cred.missing")
        : t("conn.cred.unknown");
  return (
    <Card title={t("conn.cred.title")} info="help.conn.cred">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <span>{t("conn.cred.license")}</span>
        <StateBadge tone={tone}>{label}</StateBadge>
      </div>
    </Card>
  );
}

/** Voice mode metric (read-only) plus the Chat-mode conversation. Chat turns
 * are display only: they are never typed into Kiro Crew (CP-01). */
export function VoiceModeCard({ status }: { status: BoardStatus }) {
  const { t } = useI18n();
  const chat = status.voiceMode === "chat";
  return (
    <Card title={t("conn.voice.title")} info="help.conn.voice">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <span>{t("conn.voice.mode")}</span>
        <StateBadge tone={chat ? "blue" : "neutral"}>{chat ? t("conn.voice.chat") : t("conn.voice.asr")}</StateBadge>
      </div>
      {chat && (
        <div className="kc-chat" aria-label={t("conn.voice.chatLog")}>
          {status.chatTurns.length === 0 ? (
            <div className="kc-muted">{t("conn.voice.empty")}</div>
          ) : (
            status.chatTurns.map((turn, i) => (
              <div key={i} className={`kc-chat-turn ${turn.role === "agent" ? "agent" : ""}`}>
                <b>{turn.role === "agent" ? t("conn.voice.ai") : t("conn.voice.you")}</b> {turn.text}
              </div>
            ))
          )}
        </div>
      )}
    </Card>
  );
}

/** BLE secure channel pairing (protocol.md "BLE 安全通道"). The key is made
 * on the keyboard and handed over USB only, while the pairing window is open;
 * this card shows just its id. Forget removes this PC's copy; the keyboard's
 * copy is cleared on the device (Settings → Credentials, K1 twice) or by a
 * USB ble_unpair request the keyboard confirms with K6. */
export function BlePairingCard({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  const { t } = useI18n();
  const [error, setError] = useState("");
  const canPair = usbConnected && provRemainingS(status) > 0;
  const keyboardPaired = status.keyboard?.ble_paired;
  const run = (cmd: "ble_pair" | "ble_forget" | "keyboard_ble_unpair") => {
    setError("");
    invoke(cmd).catch((e) => setError(String(e)));
  };
  return (
    <Card title={t("conn.ble.title")} info="help.conn.ble">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <span>{t("conn.ble.state")}</span>
        {status.blePairing.paired ? (
          <StateBadge tone="success">{t("conn.ble.paired", { id: status.blePairing.key_id })}</StateBadge>
        ) : (
          <StateBadge tone="neutral">{t("conn.ble.unpaired")}</StateBadge>
        )}
      </div>
      {keyboardPaired !== undefined && (
        <div className="kc-field">
          <span>{t("conn.ble.keyboardState")}</span>
          <StateBadge tone={keyboardPaired ? "success" : "neutral"}>
            {keyboardPaired ? t("conn.cred.present") : t("conn.cred.missing")}
          </StateBadge>
        </div>
      )}
      {!canPair && <div className="kc-muted kc-note">{t("conn.ble.how")}</div>}
      <div className="kc-field">
        <button type="button" className="kc-btn" disabled={!canPair} onClick={() => run("ble_pair")}>
          {status.blePairing.paired ? t("conn.ble.repair") : t("conn.ble.pair")}
        </button>
        {status.blePairing.paired && (
          <button type="button" className="kc-btn ghost" onClick={() => run("ble_forget")}>
            {t("conn.ble.forget")}
          </button>
        )}
        {keyboardPaired && (
          <button
            type="button"
            className="kc-btn ghost"
            disabled={!usbConnected}
            title={usbConnected ? undefined : t("kb.usbOnly")}
            onClick={() => run("keyboard_ble_unpair")}
          >
            {t("conn.ble.unpairKeyboard")}
          </button>
        )}
      </div>
      {keyboardPaired && !usbConnected && <div className="kc-muted kc-note">{t("kb.usbOnly")}</div>}
      {error && <StateBadge tone="warning">{error}</StateBadge>}
    </Card>
  );
}

const SEC_LABEL: Record<string, string> = {
  open: "Open",
  wep: "WEP",
  wpa: "WPA",
  wpa2: "WPA2",
  wpa_wpa2: "WPA/WPA2",
  wpa3: "WPA3",
};

/** FR-15 remembered networks. The list is read-only over Bluetooth; deleting
 * one or all needs USB. Adding one stays in the Wi-Fi form (pairing window).
 * Passwords are never shown here, they are not even sent to this PC. */
export function SavedWifiCard({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  const { t } = useI18n();
  const [error, setError] = useState("");
  const [armed, setArmed] = useState("");
  const list = status.savedWifi;
  useEffect(() => {
    invoke("wifi_saved_list").catch(() => undefined);
  }, []);
  useEffect(() => {
    if (!armed) return;
    const timer = window.setTimeout(() => setArmed(""), 4000);
    return () => window.clearTimeout(timer);
  }, [armed]);
  const run = (cmd: "wifi_saved_forget" | "wifi_saved_clear", args?: { ssid: string }) => {
    setError("");
    setArmed("");
    invoke(cmd, args).catch((e) => setError(String(e)));
  };
  const entries = list?.entries ?? [];
  return (
    <Card title={t("conn.wifi.title")} info="help.conn.wifi">
      {!list && <div className="kc-muted kc-note">{t("conn.wifi.waiting")}</div>}
      {list && entries.length === 0 && <div className="kc-muted kc-note">{t("conn.wifi.empty")}</div>}
      {entries.map((e) => (
        <div className="kc-field" key={e.ssid} style={{ marginTop: 0 }}>
          <span>{e.ssid}</span>
          {SEC_LABEL[e.sec] && <StateBadge tone="neutral">{SEC_LABEL[e.sec]}</StateBadge>}
          {e.current && <StateBadge tone="success">{t("conn.wifi.current")}</StateBadge>}
          <button
            type="button"
            className="kc-btn ghost"
            disabled={!usbConnected}
            title={usbConnected ? undefined : t("kb.usbOnly")}
            onClick={() => (armed === e.ssid ? run("wifi_saved_forget", { ssid: e.ssid }) : setArmed(e.ssid))}
          >
            {armed === e.ssid ? t("conn.wifi.confirm") : t("conn.wifi.delete")}
          </button>
        </div>
      ))}
      {list && (
        <div className="kc-muted kc-note">{t("conn.wifi.count", { n: entries.length, max: list.max || 10 })}</div>
      )}
      {entries.length > 0 && (
        <div className="kc-field">
          <button
            type="button"
            className="kc-btn ghost"
            disabled={!usbConnected}
            title={usbConnected ? undefined : t("kb.usbOnly")}
            onClick={() => (armed === "*" ? run("wifi_saved_clear") : setArmed("*"))}
          >
            {armed === "*" ? t("conn.wifi.confirm") : t("conn.wifi.clear")}
          </button>
        </div>
      )}
      {entries.length > 0 && !usbConnected && <div className="kc-muted kc-note">{t("kb.usbOnly")}</div>}
      {error && <StateBadge tone="warning">{error}</StateBadge>}
    </Card>
  );
}

/** The cards in design order, for the Connection view. */
export function BoardStatusCards({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  return (
    <div className="kc-stack">
      <KeyboardSettingsCard status={status} />
      <PairingWindowCard status={status} usbConnected={usbConnected} />
      <CredentialsCard status={status} />
      <SavedWifiCard status={status} usbConnected={usbConnected} />
      <BlePairingCard status={status} usbConnected={usbConnected} />
      <VoiceModeCard status={status} />
    </div>
  );
}

const LED_DELAYS_S = [30, 60, 300, 900];
/** LCD font sizes the keyboard offers (ak_text.h). */
const FONT_SIZES_PX = [12, 16];

/** FR-02 "Keyboard settings": the normal items the keyboard settings page
 * has, changed over USB or the paired BLE link. The keyboard applies and
 * echoes them (device_settings_state); the last change on either side wins.
 * Language, volume and brightness live in the settings menu and Device Bridge. */
export function KeyboardSettingsCard({ status }: { status: BoardStatus }) {
  const { t } = useI18n();
  const [error, setError] = useState("");
  const kb = status.keyboard;
  const send = (update: Record<string, string | number>) => {
    setError("");
    invoke("update_device_settings", { update }).catch((e) => setError(String(e)));
  };
  const seg = (on: boolean) => `kc-seg ${on ? "on" : ""}`;
  if (!kb) {
    return (
      <Card title={t("kb.title")} info="help.kb">
        <div className="kc-muted kc-note">{t("kb.waiting")}</div>
      </Card>
    );
  }
  return (
    <Card title={t("kb.title")} info="help.kb">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <span>{t("kb.voiceMode")}</span>
      </div>
      <div className="kc-keymap-head">
        {(["asr", "chat"] as const).map((m) => (
          <button key={m} type="button" className={seg(kb.voice_mode === m)} onClick={() => send({ voice_mode: m })}>
            {t(m === "asr" ? "conn.voice.asr" : "conn.voice.chat")}
          </button>
        ))}
      </div>
      <div className="kc-field">
        <span>{t("kb.ledPower")}</span>
      </div>
      <div className="kc-keymap-head">
        {(["off", "on", "auto"] as const).map((m) => (
          <button key={m} type="button" className={seg(kb.led_power === m)} onClick={() => send({ led_power: m })}>
            {t(`kb.led.${m}`)}
          </button>
        ))}
      </div>
      <div className="kc-field">
        <span>{t("kb.ledDelay")}</span>
      </div>
      <div className="kc-keymap-head">
        {LED_DELAYS_S.map((s) => (
          <button
            key={s}
            type="button"
            className={seg(kb.led_auto_off_s === s)}
            disabled={kb.led_power !== "auto"}
            onClick={() => send({ led_auto_off_s: s })}
          >
            {s < 60 ? t("kb.delay.s", { n: s }) : t("kb.delay.min", { n: s / 60 })}
          </button>
        ))}
      </div>
      {kb.font_size !== undefined && (
        <>
          <div className="kc-field">
            <span>{t("kb.fontSize")}</span>
          </div>
          <div className="kc-keymap-head">
            {FONT_SIZES_PX.map((px) => (
              <button key={px} type="button" className={seg(kb.font_size === px)} onClick={() => send({ font_size: px })}>
                {t("kb.font.px", { n: px })}
              </button>
            ))}
          </div>
        </>
      )}
      {error && <StateBadge tone="warning">{error}</StateBadge>}
    </Card>
  );
}

/** Simple mode: voice metric + the latest agent reply, one line each. */
export function SimpleVoiceLine({ status }: { status: BoardStatus }) {
  const { t } = useI18n();
  const chat = status.voiceMode === "chat";
  const last = [...status.chatTurns].reverse().find((turn) => turn.role === "agent");
  return (
    <div className="kc-stack">
      <div>
        <StateBadge tone={chat ? "blue" : "neutral"}>{chat ? t("conn.voice.chat") : t("conn.voice.asr")}</StateBadge>
      </div>
      {chat && last && (
        <div className="kc-chat-turn agent">
          <b>{t("conn.voice.ai")}</b> {last.text}
        </div>
      )}
    </div>
  );
}
