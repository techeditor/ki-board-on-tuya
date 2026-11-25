/* The read-only "Device info" rows of the Device settings view. Pure: it only
 * formats what the keyboard reported and shows "--" for anything it has not.
 *
 * Source order per row: the keyboard's `device_info` frame (USB and BLE, firmware
 * 0.3.98+), then the older `system_info` / `device_settings_state` frames (USB
 * only), so older firmware still fills what it can. Tuya PID, UUID and AuthKey are
 * shown as set / not set, never their value; the frame has no field that could hold one. */
import type { DeviceInfoFrame, SystemInfo } from "../ui/boardStatus";

export const NONE = "--";

export type InfoRow = {
  id: string;
  label: string;
  /** What the row shows. */
  value: string;
  /** What the copy button puts on the clipboard; null = nothing to copy. */
  copy: string | null;
};

export type DeviceInfoInput = {
  connected: boolean;
  /** "usb" / "ble" / "none" */
  transport: string;
  handshakeMissing: boolean;
  firmware: string;
  /** Last device_info frame (firmware 0.3.98+). */
  info: DeviceInfoFrame | null;
  /** Last system_info frame; the keyboard sends it over USB only. */
  system: SystemInfo | null;
  /** From device_settings_state; null until reported. */
  keyboard: { ble_paired?: boolean; pid?: string } | null;
  /** "present" / "missing" / "" */
  license: string;
};

type T = (key: string) => string;

/** The `device_info` board-event (flat JSON from the Rust side) as the UI keeps it. A flag the
 * frame leaves out stays null, so the row shows "--" instead of "not set". */
export function frameFromEvent(p: Partial<DeviceInfoFrame>): DeviceInfoFrame {
  return {
    fw: p.fw ?? "",
    companion: p.companion ?? "",
    handshake: p.handshake ?? null,
    cloud: p.cloud ?? "",
    wifi_ssid: p.wifi_ssid ?? "",
    wifi_ip: p.wifi_ip ?? "",
    wifi_rssi: p.wifi_rssi ?? null,
    wifi_mac: p.wifi_mac ?? "",
    ble_mac: p.ble_mac ?? "",
    ble_name: p.ble_name ?? "",
    ble_paired: p.ble_paired ?? null,
    pid: p.pid ?? null,
    uuid: p.uuid ?? null,
    authkey: p.authkey ?? null,
    license: p.license ?? null,
  };
}

/** `prefix.raw` translated; the raw value when the keyboard sends a state we have no text for. */
function stateText(t: T, prefix: string, raw: string): string {
  const key = `${prefix}.${raw}`;
  const text = t(key);
  return text === key ? raw : text;
}

function joinText(...parts: string[]): string {
  return parts.filter(Boolean).join(" · ");
}

export function deviceInfoRows(input: DeviceInfoInput, t: T): InfoRow[] {
  const { connected, keyboard } = input;
  // Nothing is shown for a keyboard that is not connected.
  const info = connected ? input.info : null;
  const system = connected ? input.system : null;
  const str = (value: string | undefined) => (connected && value ? value : "");
  const setText = (flag: boolean | null | undefined) => (flag === null || flag === undefined ? "" : t(flag ? "info.set" : "info.unset"));
  const row = (id: string, value: string, copy?: string): InfoRow => ({
    id,
    label: t(`info.${id}`),
    value: value || NONE,
    copy: (copy ?? value) || null,
  });

  const handshake = input.handshakeMissing
    ? t("info.hs.missing")
    : input.firmware
      ? t("info.hs.ok")
      : t("info.hs.pending");
  const link = connected ? joinText(input.transport.toUpperCase(), handshake) : "";

  // Cloud: device_info says active / offline / inactive; system_info says ready / unavailable / disabled.
  const cloud = info?.cloud
    ? stateText(t, "info.cloud", info.cloud)
    : system?.cloud
      ? stateText(t, "info.cloud", system.cloud)
      : "";

  // Wi-Fi name: with device_info an empty name means "not on Wi-Fi", which is worth saying.
  const ssid = info ? info.wifi_ssid : str(system?.wifi_name);
  const wifiState = info
    ? ssid
      ? ""
      : t("info.wifi.disconnected")
    : system?.wifi_state
      ? stateText(t, "info.wifi", system.wifi_state)
      : "";

  const bleName = info ? info.ble_name : str(system?.ble_name);
  const bleState = !info && system?.ble_state ? stateText(t, "info.ble", system.ble_state) : "";

  // device_info: a keyboard build without Bluetooth has neither a name nor a MAC; "paired" means nothing there.
  const hasBle = info ? Boolean(info.ble_mac || info.ble_name) : true;
  const pairedFlag = info ? (hasBle ? info.ble_paired : null) : connected ? keyboard?.ble_paired : undefined;

  const pid = info?.pid ?? (connected && keyboard?.pid ? keyboard.pid !== "missing" : null);
  // Older firmware reports only the combined state (system_info.license / device_settings_state.license:
  // UUID and AuthKey both written). With nothing else known, both rows show that same state.
  // shortcut: the older frames cannot tell a half-written pair from a missing one; it only goes away with the old firmware.
  const combined = str(input.license) ? input.license === "present" : null;
  const uuid = info?.uuid ?? combined;
  const authkey = info?.authkey ?? combined;

  return [
    row("link", link),
    row("cloud", cloud),
    row("wifi.ssid", joinText(ssid, wifiState), ssid),
    row("wifi.ip", info?.wifi_ip ?? ""),
    row("wifi.rssi", typeof info?.wifi_rssi === "number" ? `${info.wifi_rssi} dBm` : ""),
    row("wifi.mac", info?.wifi_mac ?? ""),
    row("ble.mac", info?.ble_mac ?? ""),
    row("ble.name", joinText(bleName, bleState), bleName),
    row("ble.paired", setPaired(t, pairedFlag)),
    row("fw", str(info?.fw || input.firmware)),
    row("pid", setText(pid)),
    row("uuid", setText(uuid)),
    row("authkey", setText(authkey)),
  ];
}

/** Whether the keyboard is activated on Tuya: "active" / "offline" (activated; the cloud link
 * may be down) or "inactive", or "unknown" when the keyboard has not said (no device_info frame:
 * older firmware or not connected). Same fact the keyboard uses to list "Unbind device"
 * (ak_cloud_activated()); system_info.cloud and the activation_status frames do not tell
 * activated from not, so they are not used. */
export type Activation = "active" | "inactive" | "unknown";

export function activationOf(connected: boolean, info: DeviceInfoFrame | null): Activation {
  if (!connected || !info) return "unknown";
  if (info.cloud === "inactive") return "inactive";
  if (info.cloud === "active" || info.cloud === "offline") return "active";
  return "unknown"; // an empty or new value from newer firmware: do not hide anything
}

/** "Unbind device" is shown like the keyboard does: only when activated, or when that is not known. */
export function showUnbind(connected: boolean, info: DeviceInfoFrame | null): boolean {
  return activationOf(connected, info) !== "inactive";
}

function setPaired(t: T, flag: boolean | null | undefined): string {
  return flag === null || flag === undefined ? "" : t(flag ? "info.paired" : "info.unpaired");
}
