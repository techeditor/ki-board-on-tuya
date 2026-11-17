/* Board-reported state for the Connection view: pairing window (prov_status),
 * License presence (system_info.license), voice mode (voice_source.mode), the
 * Chat-mode conversation (tuya_chat) and the BLE pairing stored on this PC. Shared by the production entry
 * (src/main.tsx) and the redesign shell (src/app/RedesignApp.tsx).
 *
 * Without the Tauri runtime (the redesign.html browser preview) no listener is
 * registered and the defaults are returned, so the cards still render. */
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

export type ChatTurn = { role: string; text: string };

export interface ProvRejection {
  /** Increments on every refusal so a consumer can react once per event. */
  seq: number;
  command: string;
  reason: string;
}

export interface BoardStatus {
  /** Wall-clock deadline of the open pairing window (ms), 0 when closed. */
  provUntilMs: number;
  nowMs: number;
  rejection: ProvRejection;
  /** "present" / "missing" from system_info; "" until the board reports. */
  license: string;
  /** "asr" / "chat" from voice_source.mode. */
  voiceMode: string;
  chatTurns: ChatTurn[];
  /** BLE pairing stored on this PC (ble_pairing_status / ble-pairing). Only
   * the key id is known here; the key itself never reaches the frontend. */
  blePairing: BlePairing;
  /** FR-01 interface language from device_settings_state ("zh" / "en"); "" until reported. */
  language: string;
  /** FR-02 keyboard settings from device_settings_state; null until reported
   * (or from firmware older than FR-02). */
  keyboard: KeyboardSettings | null;
  /** FR-15 remembered networks from wifi_list_state; null until reported. */
  savedWifi: SavedWifiList | null;
}

/** Normal keyboard settings plus sensitive-item status (FR-02). */
export interface KeyboardSettings {
  voice_mode?: string; // "asr" / "chat"
  led_power?: string; // "off" / "on" / "auto"
  led_auto_off_s?: number;
  font_size?: number; // LCD font size in px: 12 / 16 (absent on older firmware)
  license?: string; // "present" / "missing"
  ble_paired?: boolean; // the keyboard's copy of the pairing key
}

/** One remembered Wi-Fi network (FR-15). The keyboard never reports a password. */
export interface SavedWifi {
  ssid: string;
  sec: string; // "open" / "wep" / "wpa" / "wpa2" / "wpa_wpa2" / "wpa3" / "unknown"
  current: boolean;
}

export interface SavedWifiList {
  max: number;
  entries: SavedWifi[];
}

export interface BlePairing {
  paired: boolean;
  key_id: string;
}

const MAX_TURNS = 20;

function tauriAvailable(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

export function useBoardStatus(): BoardStatus {
  const [provUntilMs, setProvUntilMs] = useState(0);
  const [nowMs, setNowMs] = useState(() => Date.now());
  const [rejection, setRejection] = useState<ProvRejection>({ seq: 0, command: "", reason: "" });
  const [license, setLicense] = useState("");
  const [voiceMode, setVoiceMode] = useState("asr");
  const [chatTurns, setChatTurns] = useState<ChatTurn[]>([]);
  const [blePairing, setBlePairing] = useState<BlePairing>({ paired: false, key_id: "" });
  const [language, setLanguage] = useState("");
  const [keyboard, setKeyboard] = useState<KeyboardSettings | null>(null);
  const [savedWifi, setSavedWifi] = useState<SavedWifiList | null>(null);

  useEffect(() => {
    if (!tauriAvailable()) return;
    void invoke<BlePairing>("ble_pairing_status")
      .then(setBlePairing)
      .catch(() => undefined);
    const subs: Promise<UnlistenFn>[] = [
      listen<BlePairing>("ble-pairing", (e) => setBlePairing(e.payload)),
      listen<{ state: string; remaining_s: number; rejected: string; reason: string }>("prov-status", (e) => {
        const p = e.payload;
        const open = p.state === "open" && p.remaining_s > 0;
        setNowMs(Date.now());
        setProvUntilMs(open ? Date.now() + p.remaining_s * 1000 : 0);
        if (p.rejected) {
          setRejection((r) => ({ seq: r.seq + 1, command: p.rejected, reason: p.reason }));
        }
      }),
      listen<{ kind?: string; license?: string }>("board-event", (e) => {
        if (e.payload.kind === "system_info" && typeof e.payload.license === "string") {
          setLicense(e.payload.license);
        }
      }),
      listen<string>("voice-mode", (e) => setVoiceMode(e.payload)),
      listen<SavedWifiList>("wifi-saved", (e) => setSavedWifi(e.payload)),
      listen<{ language?: string } & KeyboardSettings>("device-settings", (e) => {
        setLanguage(e.payload.language ?? "");
        const p = e.payload;
        if (p.voice_mode !== undefined || p.led_power !== undefined || p.font_size !== undefined) {
          setKeyboard({
            voice_mode: p.voice_mode,
            led_power: p.led_power,
            led_auto_off_s: p.led_auto_off_s,
            font_size: p.font_size,
            license: p.license,
            ble_paired: p.ble_paired,
          });
          if (p.voice_mode) setVoiceMode(p.voice_mode);
          if (p.license) setLicense(p.license);
        }
      }),
      listen<ChatTurn>("tuya-chat", (e) => {
        const { role, text } = e.payload;
        setChatTurns((turns) => [...turns, { role, text }].slice(-MAX_TURNS));
      }),
    ];
    return () => {
      subs.forEach((s) => void s.then((off) => off()));
    };
  }, []);

  // Tick once a second only while the window is open (local countdown).
  useEffect(() => {
    if (provUntilMs <= Date.now()) return;
    const timer = window.setInterval(() => setNowMs(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [provUntilMs]);

  return { provUntilMs, nowMs, rejection, license, voiceMode, chatTurns, blePairing, language, keyboard, savedWifi };
}

/** Seconds left in the pairing window, 0 when closed. */
export function provRemainingS(s: BoardStatus): number {
  return Math.max(0, Math.ceil((s.provUntilMs - s.nowMs) / 1000));
}

/** i18n key explaining a prov_status refusal reason. */
export function provReasonKey(reason: string): string {
  switch (reason) {
    case "closed":
    case "exists":
    case "confirm_on_device":
    case "cancelled":
    case "busy":
      return `conn.pairing.reason.${reason}`;
    default:
      return "conn.pairing.reason.other";
  }
}
