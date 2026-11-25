/* Types and pure helpers shared by the redesign model and its views. They
 * mirror the Rust backend (src-tauri/src/lib.rs); no logic lives here that
 * touches Tauri, so the views and the model can both import it. */
import { create as createQrCode } from "qrcode";

export type Transport = "none" | "usb" | "ble";

export type Status = {
  device_connected: boolean;
  transport: Transport;
  endpoint: string;
  firmware_version: string;
  last_error: string;
  control_enabled?: boolean;
  focus_on_select?: boolean;
  show_workers?: boolean;
  voice_force_keyboard?: boolean;
  device_volume: number;
  device_brightness: number;
  device_settings_synced: boolean;
  crew_api_connected?: boolean;
  crew_api_base_url?: string;
  crew_api_last_error?: string;
  crew_api_slots?: number;
  /** Connected, but the keyboard did not answer `hello` within about 3 s. */
  handshake_missing?: boolean;
};

export type CrewSlot = {
  key: string;
  title: string;
  agent: string;
  running: boolean;
  stopping: boolean;
  pending_approval: boolean;
  waiting_for_input: boolean;
  needs_input: boolean;
  subagents_running: boolean;
  last_message: string;
};

export type CrewConnection = {
  connected: boolean;
  base_url: string;
  error: string;
  retry_secs: number;
};

export type DeviceSettings = {
  volume: number;
  brightness: number;
  /** FR-01 interface language; absent from older firmware. */
  language?: string;
  /** Tuya product ID state: "present" / "pending" (written, restart to apply) /
   * "missing"; absent from older firmware. The value itself is never reported. */
  pid?: string;
};

/** Result of a send_pid write (pid-status event). */
export type PidStatus = { state: string; reason: string };

export type LicenseInfo = {
  source: string;
  uuid: string;
  authkey_tail: string;
};

export type ActivationStatus = {
  state: string;
  qr_url: string;
};

export type SerialPortInfo = { name: string; kind: string };
export type BleDeviceInfo = { id: string; name: string };

/** Flat board-event object emitted by `inbound_to_json` in the backend. */
export type BoardEvent = {
  kind: string;
  id?: string;
  action?: string;
  bind?: string | null;
  payload?: unknown;
  [field: string]: unknown;
};

/** FP-4b STATUS: a mapped agent status pushed from the hook sink via `kc-status`. */
export type KcStatus = {
  session_id: string;
  agent_name: string;
  state: string;
  text: string;
  event?: string;
  detail_text?: string;
  forwarded?: boolean;
};

/** FP-4b: one-click user-level global Crew hook install status. */
export type HookStatus = {
  command: string;
  hooks_dir: string;
  hook_file: string;
  installed_events: number;
  fully_installed: boolean;
  any_installed: boolean;
};

/** FP-4b M4 (§3.2): A/B/C slot numbers, aliases and stable session keys. */
export type ChatNumbers = number[];
export type ChatAliases = string[];
export type ChatSessions = string[];
export type WatchedView = "compact" | "detail";
export type QuickBindDraft = { sessionId: string; slot: number; number: number };

export const SLOT_DEFAULT_NAMES = ["A", "B", "C"] as const;
export const ALIAS_MAX_CHARS = 7;

/** CP-09: the Rust side prefixes every gateway identity failure with this. */
export const IDENTITY_FAILED_PREFIX = "Crew identity check failed";
export function isIdentityFailure(error: string): boolean {
  return error.startsWith(IDENTITY_FAILED_PREFIX);
}

export function sessionDisplayName(sessionId: string): string {
  const firstPart = sessionId.split(":", 1)[0]?.trim();
  return firstPart || "Kiro Crew";
}

export const defaultStatus: Status = {
  device_connected: false,
  transport: "none",
  endpoint: "",
  firmware_version: "",
  last_error: "",
  control_enabled: true,
  focus_on_select: true,
  voice_force_keyboard: false,
  device_volume: 70,
  device_brightness: 100,
  device_settings_synced: false,
  crew_api_connected: false,
  crew_api_base_url: "http://127.0.0.1:5476",
  crew_api_last_error: "",
  crew_api_slots: 0,
};

export const ACTIVATION_STEPS = ["router", "scan", "confirm", "ready"] as const;

/** Index into ACTIVATION_STEPS for the current SmartLife state, -1 when none. */
export function activationStepIndex(state: string): number {
  switch (state) {
    case "connecting_router":
    case "router_failed":
      return 0;
    case "wifi_connected":
    case "scan_qr":
      return 1;
    case "token_received":
    case "activated":
    case "cloud_disconnected":
      return 2;
    case "cloud_ready":
      return 3;
    default:
      return -1;
  }
}

/** i18n key for the SmartLife state badge; unknown states use the pre-setup text. */
export function activationLabelKey(state: string): string {
  const known = [
    "connecting_router",
    "router_failed",
    "wifi_connected",
    "pairing",
    "scan_qr",
    "token_received",
    "activated",
    "cloud_ready",
    "cloud_disconnected",
    "resetting",
    "disabled",
  ];
  return `act.state.${known.includes(state) ? state : "pairing"}`;
}

export type QrDrawing = { size: number; path: string };

export function buildQrDrawing(url: string): QrDrawing | null {
  if (!url.startsWith("https://smartapp.tuya.com/s/p?")) return null;
  const qr = createQrCode(url, { errorCorrectionLevel: "M" });
  const size = qr.modules.size;
  let path = "";
  for (let row = 0; row < size; row += 1) {
    for (let col = 0; col < size; col += 1) {
      if (qr.modules.get(row, col)) path += `M${col} ${row}h1v1h-1z`;
    }
  }
  return { size, path };
}

/** Saved link choice (connection.json). Mirrors connection_prefs::Prefs in Rust. */
export type ConnPrefs = {
  /** "auto" (USB first, then the paired BLE device), "usb" or "ble". */
  transport: "auto" | "usb" | "ble";
  /** Named serial port; empty = auto-detect the CH34x board port. */
  port: string;
  /** Named or last-connected BLE device id; empty = none. */
  ble_device: string;
  auto_connect: boolean;
};

export const defaultConnPrefs: ConnPrefs = { transport: "auto", port: "", ble_device: "", auto_connect: true };
