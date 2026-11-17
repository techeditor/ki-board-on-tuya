/* Connection-view cards for the board's pairing window, credentials and voice
 * mode (redesign tasks T4.1; design §1 "Connection"). Theme tokens, i18n and
 * InfoTip only — explanations live in help.* popovers (R5). Pairing window,
 * License and voice mode are read-only: they are changed on the keyboard.
 * The BLE card can request a new pairing key over USB (window open) or forget
 * this PC's copy. */
import { useState } from "react";
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
  const left = provRemainingS(status);
  const open = usbConnected && left > 0;
  const rejected = status.rejection.seq > 0 && status.rejection.reason !== "confirm_on_device";
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
 * copy is cleared on the device (Settings → Credentials, K1 twice). */
export function BlePairingCard({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  const { t } = useI18n();
  const [error, setError] = useState("");
  const canPair = usbConnected && provRemainingS(status) > 0;
  const run = (cmd: "ble_pair" | "ble_forget") => {
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
      </div>
      {error && <StateBadge tone="warning">{error}</StateBadge>}
    </Card>
  );
}

/** The cards in design order, for the Connection view. */
export function BoardStatusCards({ status, usbConnected }: { status: BoardStatus; usbConnected: boolean }) {
  return (
    <div className="kc-stack">
      <PairingWindowCard status={status} usbConnected={usbConnected} />
      <CredentialsCard status={status} />
      <BlePairingCard status={status} usbConnected={usbConnected} />
      <VoiceModeCard status={status} />
    </div>
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
