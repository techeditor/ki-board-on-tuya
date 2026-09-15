import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { createRoot } from "react-dom/client";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./styles.css";

// ─── Types mirroring the Rust backend (src-tauri/src/lib.rs) ────────────────

type Transport = "none" | "usb" | "ble";

type Status = {
  device_connected: boolean;
  transport: Transport;
  endpoint: string;
  firmware_version: string;
  last_error: string;
  control_enabled?: boolean;
  device_volume: number;
  device_brightness: number;
  device_settings_synced: boolean;
};

type DeviceSettings = {
  volume: number;
  brightness: number;
};

type LicenseInfo = {
  source: string;
  uuid: string;
  authkey_tail: string;
};

type SerialPortInfo = {
  name: string;
  kind: string;
};

type BleDeviceInfo = {
  id: string;
  name: string;
};

// Flat board-event object emitted by `inbound_to_json` in the backend.
type BoardEvent = {
  kind: "key" | "nav" | "voice" | "macro" | "license_info" | "device_settings" | "system_info" | "hello_ack" | "ping" | "pong" | "unknown";
  seq?: number | null;
  id?: string;
  dir?: string;
  action?: string;
  held_ms?: number;
  state?: string;
  name?: string;
  volume?: number;
  brightness?: number;
  fw?: string;
  transport?: string;
  wifi_name?: string;
  wifi_state?: string;
  ble_name?: string;
  ble_state?: string;
  cloud?: string;
  license?: string;
  source?: string;
  uuid?: string;
  authkey_tail?: string;
  type?: string;
};

// FP-4b STATUS: a mapped agent status pushed from the hook sink via `kc-status`.
type KcStatus = {
  session_id: string;
  agent_name: string;
  state: string;
  text: string;
  event?: string;
  detail_text?: string;
  // Set by the sink: true when this session is the one currently driving the
  // board LCD (either nothing is pinned, or this is the pinned session).
  forwarded?: boolean;
};

// FP-4b: one-click user-level global Crew hook install status.
type HookStatus = {
  command: string;
  hooks_dir: string;
  hook_file: string;
  installed_events: number;
  fully_installed: boolean;
  any_installed: boolean;
};

// FP-4b M4 (§3.2): each A/B/C slot has a chat shortcut number and an optional
// AK display alias. Empty aliases fall back to A, B, and C on the board.
type ChatNumbers = number[];
type ChatAliases = string[];
type ChatSessions = string[];
type WatchedView = "compact" | "detail";
type QuickBindDraft = { sessionId: string; slot: number; number: number };

const SLOT_DEFAULT_NAMES = ["A", "B", "C"] as const;
const ALIAS_MAX_CHARS = 7;

function sessionDisplayName(sessionId: string) {
  const firstPart = sessionId.split(":", 1)[0]?.trim();
  return firstPart || "Kiro Crew";
}

const defaultStatus: Status = {
  device_connected: false,
  transport: "none",
  endpoint: "",
  firmware_version: "",
  last_error: "",
  control_enabled: true,
  device_volume: 70,
  device_brightness: 100,
  device_settings_synced: false,
};

// Our 5 mechanical keys (k1..k5) with their phase-1 default intents. This is a
// reference mapping placeholder — remapping UI lands in a later pass. `wide`
// marks K4 as the double-width push-to-talk key in the INPUT layout.
const KEY_MAP: { id: string; label: string; intent: string; wide?: boolean }[] = [
  { id: "k1", label: "K1", intent: "Chat A" },
  { id: "k2", label: "K2", intent: "Chat B" },
  { id: "k3", label: "K3", intent: "Chat C" },
  { id: "k4", label: "K4", intent: "Push-to-Talk (voice)", wide: true },
  { id: "k5", label: "K5", intent: "Submit / Continue" },
];

// 3×3 grid positions for the compact 5-way navigation cluster.
const NAV_LAYOUT: (string | null)[] = [
  null, "up", null,
  "left", "center", "right",
  null, "down", null,
];

function App() {
  const [status, setStatus] = useState<Status>(defaultStatus);
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [selectedPort, setSelectedPort] = useState("");
  const [transportMode, setTransportMode] = useState<"usb" | "ble">("usb");
  const [bleDevices, setBleDevices] = useState<BleDeviceInfo[]>([]);
  const [selectedBle, setSelectedBle] = useState("");
  const [bleScanning, setBleScanning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  // Raw board-frame JSON stream, shown when the Logs "raw" toggle is on. The
  // `logs` array is the human-readable/detailed view ([RX] key k4 …); `logsRaw`
  // is the untouched JSON object per board-event.
  const [logsRaw, setLogsRaw] = useState<string[]>([]);
  const [logsShowRaw, setLogsShowRaw] = useState(false);
  // LOGS pause/resume: while paused the visible list is FROZEN (so the user can
  // observe and copy without it scrolling); new lines are buffered in refs and
  // flushed back on resume. A ref mirrors the flag so the once-registered event
  // listeners read the live value instead of a stale closure.
  const [logsPaused, setLogsPaused] = useState(false);
  const logsPausedRef = useRef(false);
  const logBufRef = useRef<string[]>([]);
  const rawBufRef = useRef<string[]>([]);
  const [pausedCount, setPausedCount] = useState(0);
  const [voiceSource, setVoiceSource] = useState<"cloud" | "system">("cloud");
  // WiFi provisioning (DEVICE panel): send SSID/pass to the board over the link.
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [showWifiPass, setShowWifiPass] = useState(false);
  const [wifiStatus, setWifiStatus] = useState("");
  const [wifiBusy, setWifiBusy] = useState(false);
  // Device license (UUID/AuthKey) provisioning.
  const [licUuid, setLicUuid] = useState("");
  const [licAuthkey, setLicAuthkey] = useState("");
  const [licStatus, setLicStatus] = useState("");
  const [licBusy, setLicBusy] = useState(false);
  const [licInfo, setLicInfo] = useState<LicenseInfo | null>(null);
  const [pairingBusy, setPairingBusy] = useState(false);
  const [pairingStatus, setPairingStatus] = useState("");
  const [deviceSettings, setDeviceSettings] = useState<DeviceSettings>({
    volume: defaultStatus.device_volume,
    brightness: defaultStatus.device_brightness,
  });
  const [deviceSettingsBusy, setDeviceSettingsBusy] = useState(false);
  const [lastKey, setLastKey] = useState("");
  const [lastNav, setLastNav] = useState("");
  const [voiceActive, setVoiceActive] = useState(false);
  const [activeView, setActiveView] = useState<"device" | "input" | "kirocrew" | "logs">("device");
  const [uiError, setUiError] = useState("");
  const statusPollInFlightRef = useRef(false);
  const usbProvisioningAvailable = status.device_connected && status.transport === "usb";

  // FP-4b route B ("Local Control", the hands) state: focus a Kiro Crew window
  // and inject keystrokes.
  const [lcTitle, setLcTitle] = useState("");
  const [lcBound, setLcBound] = useState(false);
  const [lcText, setLcText] = useState("");
  const [lcResult, setLcResult] = useState("");
  const [lcError, setLcError] = useState("");
  const [lcBusy, setLcBusy] = useState(false);

  // FP-4b M4 (§3.2): A/B/C chat numbers and optional AK display aliases.
  // Aliases save on blur/Enter rather than on every keystroke.
  const [binds, setBinds] = useState<ChatNumbers>([]);
  const [bindAliases, setBindAliases] = useState<ChatAliases>(["", "", ""]);
  const [bindSessions, setBindSessions] = useState<ChatSessions>(["", "", ""]);
  const [bindError, setBindError] = useState("");
  const [bindBusy, setBindBusy] = useState(false);

  // FP-4b STATUS sink ("the eyes"): a localhost HTTP ingress for Kiro agent
  // hooks (delivered by `companion --hook`). Shows the token + hook command to
  // paste, and a live per-session status list from the `kc-status` UI event.
  const [ssRunning, setSsRunning] = useState(false);
  const [ssToken, setSsToken] = useState("");
  const [ssError, setSsError] = useState("");
  const [ssBusy, setSsBusy] = useState(false);
  const [kcStatuses, setKcStatuses] = useState<Record<string, KcStatus>>({});
  const [watchedView, setWatchedView] = useState<WatchedView>("compact");
  const [quickBindDraft, setQuickBindDraft] = useState<QuickBindDraft | null>(null);
  // FP-4b Route STATUS (option A): the pinned "watched chat". "" = follow all
  // sessions. Non-empty = only that session's status drives the board LCD.
  const [pinnedSession, setPinnedSession] = useState("");
  // FP-4b: Crew ScriptHookStore hooks apply globally to dashboard agent turns.
  const [hookStatus, setHookStatus] = useState<HookStatus | null>(null);

  const boardPorts = useMemo(
    () => ports.filter((port) => port.kind !== "flash"),
    [ports],
  );
  const sortedKcStatuses = useMemo(
    () =>
      Object.values(kcStatuses).sort((a, b) =>
        a.session_id === pinnedSession
          ? -1
          : b.session_id === pinnedSession
            ? 1
            : a.session_id.localeCompare(b.session_id),
      ),
    [kcStatuses, pinnedSession],
  );

  async function refreshRuntime() {
    const runtime = await invoke<Status>("get_status");
    setStatus(runtime);
    setDeviceSettings({
      volume: runtime.device_volume,
      brightness: runtime.device_brightness,
    });
    setPorts(await invoke<SerialPortInfo[]>("list_serial_ports"));
    // Don't clobber the frozen list while the LOGS view is paused.
    if (!logsPausedRef.current) {
      setLogs(await invoke<string[]>("get_logs"));
    }
  }

  async function runAction(action: () => Promise<void>) {
    try {
      setUiError("");
      await action();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setUiError(message);
    }
  }

  async function connect() {
    setLicInfo(null);
    if (transportMode === "ble") {
      if (!selectedBle) {
        throw new Error("Scan and select an AK-KB device first.");
      }
      await invoke("connect_device", { transport: "ble", portName: selectedBle });
    } else {
      await invoke("connect_device", { transport: "usb", portName: selectedPort || null });
    }
    await refreshRuntime();
  }

  async function scanBle() {
    setBleScanning(true);
    try {
      const found = await invoke<BleDeviceInfo[]>("scan_ble");
      setBleDevices(found);
      // Auto-select the first match so Connect is one click after a scan.
      if (found.length > 0 && !found.some((d) => d.id === selectedBle)) {
        setSelectedBle(found[0].id);
      }
    } finally {
      setBleScanning(false);
    }
  }

  async function disconnect() {
    await invoke("disconnect_device");
    setLicInfo(null);
    await refreshRuntime();
  }

  async function adjustDeviceSetting(key: keyof DeviceSettings, delta: number) {
    const minimum = key === "brightness" ? 10 : 0;
    const current = deviceSettings[key];
    const nextValue = Math.max(minimum, Math.min(100, current + delta));
    if (nextValue === current) return;

    const next = { ...deviceSettings, [key]: nextValue };
    setDeviceSettingsBusy(true);
    try {
      await invoke("set_device_settings", next);
    } finally {
      setDeviceSettingsBusy(false);
    }
  }

  async function clearLogs() {
    await invoke("clear_logs");
    setLogs([]);
    setLogsRaw([]);
    logBufRef.current = [];
    rawBufRef.current = [];
    setPausedCount(0);
  }

  // Toggle LOGS pause. On resume, flush the buffered lines (newest-first) back
  // onto the head of each list so nothing observed while paused is lost.
  function toggleLogsPause() {
    if (logsPausedRef.current) {
      logsPausedRef.current = false;
      setLogsPaused(false);
      if (logBufRef.current.length) {
        const buf = logBufRef.current;
        setLogs((items) => [...buf, ...items].slice(0, 200));
        logBufRef.current = [];
      }
      if (rawBufRef.current.length) {
        const buf = rawBufRef.current;
        setLogsRaw((items) => [...buf, ...items].slice(0, 200));
        rawBufRef.current = [];
      }
      setPausedCount(0);
    } else {
      logsPausedRef.current = true;
      setLogsPaused(true);
    }
  }

  // Enable/disable routing board keys to Kiro Crew control. Default ON so the
  // Companion is ready to drive Kiro Crew immediately; disabling it still
  // gates every board-triggered PC action, including K4 / Win+H.
  async function toggleControl(enabled: boolean) {
    await invoke("set_control_enabled", { enabled });
    setStatus((s) => ({ ...s, control_enabled: enabled }));
  }

  async function selectVoiceSource(source: "cloud" | "system") {
    setVoiceSource(source);
    // set_voice_source updates companion state AND mirrors the choice to the
    // board via a config frame. Ignore the "not connected" error when offline.
    await invoke("set_voice_source", { source }).catch(() => undefined);
  }

  async function sendWifi() {
    if (!usbProvisioningAvailable) {
      setUiError("Connect the board over USB before configuring WiFi.");
      return;
    }
    if (!wifiSsid.trim()) {
      setUiError("Enter a WiFi SSID first.");
      return;
    }
    setWifiBusy(true);
    try {
      setUiError("");
      setWifiStatus("sending…");
      await invoke("send_wifi", { ssid: wifiSsid.trim(), pass: wifiPass });
      setWifiStatus("sent, waiting for board…");
    } catch (error) {
      setWifiStatus("");
      setUiError(error instanceof Error ? error.message : String(error));
    } finally {
      setWifiBusy(false);
    }
  }

  async function sendLicense() {
    if (!usbProvisioningAvailable) {
      setUiError("Connect the board over USB before writing UUID/AuthKey.");
      return;
    }
    if (!licUuid.trim() || !licAuthkey.trim()) {
      setUiError("UUID and AuthKey are both required.");
      return;
    }
    setLicBusy(true);
    try {
      setUiError("");
      setLicStatus("writing…");
      await invoke("send_license", { uuid: licUuid.trim(), authkey: licAuthkey.trim() });
      setLicStatus("sent, waiting for board…");
    } catch (error) {
      setLicStatus("");
      setUiError(error instanceof Error ? error.message : String(error));
    } finally {
      setLicBusy(false);
    }
  }

  async function readLicense() {
    if (!usbProvisioningAvailable) {
      setUiError("Connect the board over USB before reading License diagnostics.");
      return;
    }
    setLicBusy(true);
    try {
      setUiError("");
      setLicStatus("reading…");
      setLicInfo(null);
      await invoke("read_license");
      setLicStatus("read request sent…");
    } catch (error) {
      setLicStatus("");
      setUiError(error instanceof Error ? error.message : String(error));
    } finally {
      setLicBusy(false);
    }
  }

  // One-click "enter pairing": clears the board's Wi-Fi + cloud binding and
  // reboots it into provisioning. The License (UUID/AuthKey) is preserved.
  // Destructive + reboots the board, so require an explicit confirmation and
  // gate on USB (matches wifi/license).
  async function enterPairing() {
    if (!usbProvisioningAvailable) {
      setUiError("Connect the board over USB before entering pairing mode.");
      return;
    }
    const ok = window.confirm(
      "Enter pairing mode?\n\nThis clears the keyboard's saved Wi-Fi and cloud " +
        "binding and reboots it into provisioning. The device License " +
        "(UUID/AuthKey) is kept. You will need to re-provision Wi-Fi afterwards.",
    );
    if (!ok) {
      return;
    }
    setPairingBusy(true);
    try {
      setUiError("");
      setPairingStatus("sending…");
      await invoke("enter_pairing");
      setPairingStatus("sent — keyboard will reboot into pairing mode");
    } catch (error) {
      setPairingStatus("");
      setUiError(error instanceof Error ? error.message : String(error));
    } finally {
      setPairingBusy(false);
    }
  }

  // ── FP-4b route B: Local Control (window focus + keystroke injection) ──────
  // Arg names match the #[tauri::command] signatures in src-tauri/src/lib.rs:
  //   kc_bind_window(title)   kc_send_text(text)
  //   kc_command_bar(cmd)     kc_chord(name)
  async function runLc(action: () => Promise<void>) {
    setLcBusy(true);
    try {
      setLcError("");
      await action();
    } catch (error) {
      setLcError(error instanceof Error ? error.message : String(error));
      setLcBound(false);
      setLcResult("bind/inject failed");
    } finally {
      setLcBusy(false);
    }
  }

  async function lcBind() {
    // Empty title => backend tries the default candidates ("Kiro Crew"/"Agentic").
    await invoke("kc_bind_window", { title: lcTitle.trim() });
    setLcBound(true);
    setLcResult(`focused ${lcTitle.trim() || "Kiro Crew / Agentic"}`);
  }

  async function lcSendText() {
    const text = lcText.trim();
    if (!text) {
      setLcError("Type some text to send.");
      return;
    }
    await invoke("kc_send_text", { text });
    setLcResult(`sent text (${text.length} chars)`);
  }

  async function lcCommandBar() {
    const cmd = lcText.trim();
    if (!cmd) {
      setLcError("Type a command for the command bar.");
      return;
    }
    await invoke("kc_command_bar", { cmd });
    setLcResult(`command bar: ${cmd}`);
    setLcText("");
  }

  async function lcChord(name: string) {
    await invoke("kc_chord", { name });
    setLcResult(`chord: ${name}`);
  }

  // ── FP-4b M4: per-key chat numbers and A/B/C aliases ──────────────────────
  // Arg names match the #[tauri::command] signatures in src-tauri/src/lib.rs:
  //   kc_bind_get() -> number[]      kc_bind_set(slot, number)
  //   kc_bind_aliases_get() -> string[]
  //   kc_bind_alias_set(slot, alias) -> string[]
  //   kc_bind_clear(slot)
  async function runBind(action: () => Promise<void>) {
    setBindBusy(true);
    try {
      setBindError("");
      await action();
    } catch (error) {
      setBindError(error instanceof Error ? error.message : String(error));
    } finally {
      setBindBusy(false);
    }
  }

  async function refreshBinds() {
    const [numbers, aliases, sessions] = await Promise.all([
      invoke<ChatNumbers>("kc_bind_get"),
      invoke<ChatAliases>("kc_bind_aliases_get"),
      invoke<ChatSessions>("kc_bind_sessions_get"),
    ]);
    setBinds(numbers);
    setBindAliases(aliases);
    setBindSessions(sessions);
  }

  // Set A/B/C slot `slot` (1-based API index) to chat `number` (1..9).
  async function bindSet(slot: number, number: number) {
    await invoke("kc_bind_set", { slot, number });
    await refreshBinds();
  }

  async function bindSession(slot: number, number: number, sessionId: string) {
    await invoke("kc_bind_session_set", { slot, number, sessionId });
    await refreshBinds();
  }

  async function clearBoundSession(slot: number) {
    await invoke("kc_bind_session_clear", { slot });
    await refreshBinds();
    setQuickBindDraft(null);
  }

  function startQuickBind(sessionId: string, slot: number) {
    setQuickBindDraft({ sessionId, slot, number: binds[slot - 1] ?? slot });
  }

  async function confirmQuickBind() {
    if (!quickBindDraft) return;
    const draft = quickBindDraft;
    await runBind(async () => {
      await bindSession(draft.slot, draft.number, draft.sessionId);
      setQuickBindDraft(null);
    });
  }

  function bindAliasDraft(slot: number, alias: string) {
    setBindAliases((current) => {
      const next = [...current];
      next[slot - 1] = alias;
      return next;
    });
  }

  async function bindAliasSave(slot: number, alias: string) {
    const aliases = await invoke<ChatAliases>("kc_bind_alias_set", { slot, alias });
    setBindAliases(aliases);
  }

  // Reset the chat number only; the A/B/C slot alias remains independent.
  async function bindClear(slot: number) {
    await invoke("kc_bind_clear", { slot });
    await refreshBinds();
  }

  // ── FP-4b STATUS sink (the eyes) ──────────────────────────────────────────
  // Arg names match the #[tauri::command] signatures in src-tauri/src/lib.rs:
  //   status_sink_start(port?) -> String (token)   status_sink_stop()
  //   status_token() -> String
  async function runSs(action: () => Promise<void>) {
    setSsBusy(true);
    try {
      setSsError("");
      await action();
    } catch (error) {
      setSsError(error instanceof Error ? error.message : String(error));
    } finally {
      setSsBusy(false);
    }
  }

  async function ssStart() {
    const token = await invoke<string>("status_sink_start", { port: null });
    setSsToken(token);
    setSsRunning(true);
  }

  async function ssStop() {
    await invoke("status_sink_stop");
    setSsRunning(false);
    setSsToken("");
  }

  // Pin one session as the watched chat (only its status drives the board), or
  // pass "" to unpin and follow every session again.
  async function pinSession(sessionId: string) {
    await invoke("kc_pin_session", { sessionId });
    setPinnedSession(sessionId);
  }

  async function unpinSession() {
    await invoke("kc_unpin_session");
    setPinnedSession("");
  }

  // FP-4b: global Crew ScriptHookStore install/remove/status.
  async function hookRefresh() {
    setHookStatus(await invoke<HookStatus>("kc_hook_status", { exe: null }));
  }

  async function hookOpenFolder() {
    await invoke<string>("kc_open_hooks_folder");
  }

  async function hookInstall() {
    setHookStatus(await invoke<HookStatus>("kc_hook_install", { exe: null }));
  }

  async function hookRemove() {
    setHookStatus(await invoke<HookStatus>("kc_hook_remove", { exe: null }));
  }

  useEffect(() => {
    refreshRuntime().catch(() => undefined);
    const timer = window.setInterval(() => {
      // Never pile up status requests behind a blocked backend mutex. One
      // outstanding poll is enough; the persistent backend watchdog records
      // lock stalls for post-mortem diagnosis.
      if (statusPollInFlightRef.current) return;
      statusPollInFlightRef.current = true;
      invoke<Status>("get_status")
        .then(setStatus)
        .catch((error) => console.warn("get_status failed", error))
        .finally(() => {
          statusPollInFlightRef.current = false;
        });
    }, 1500);

    const unlistenLog = listen<string>("companion-log", (event) => {
      if (logsPausedRef.current) {
        logBufRef.current = [event.payload, ...logBufRef.current].slice(0, 200);
        setPausedCount((n) => n + 1);
        return;
      }
      setLogs((items) => [event.payload, ...items].slice(0, 200));
    });

    const unlistenBoard = listen<BoardEvent>("board-event", (event) => {
      const payload = event.payload;
      if (logsPausedRef.current) {
        rawBufRef.current = [JSON.stringify(payload), ...rawBufRef.current].slice(0, 200);
      } else {
        setLogsRaw((items) => [JSON.stringify(payload), ...items].slice(0, 200));
      }
      if (payload.kind === "key" && payload.id) {
        if (payload.action === "down" || payload.action === "long") {
          setLastKey(payload.id);
        } else if (payload.action === "up") {
          // Release clears the highlight so the INPUT view reflects only the
          // current physical trigger state, not the last-pressed key.
          setLastKey((cur) => (cur === payload.id ? "" : cur));
        }
      } else if (payload.kind === "nav" && payload.dir) {
        if (payload.action === "down" || payload.action === "long") {
          setLastNav(payload.dir);
        } else if (payload.action === "up") {
          setLastNav((cur) => (cur === payload.dir ? "" : cur));
        }
      } else if (payload.kind === "voice") {
        setVoiceActive(payload.state === "start");
      }
    });

    // FP-4b STATUS: live per-session agent status pushed by the hook sink.
    const unlistenStatus = listen<KcStatus>("kc-status", (event) => {
      const s = event.payload;
      setKcStatuses((prev) => ({ ...prev, [s.session_id]: s }));
    });

    // The board reports its current voice routing (cloud/system) on change and
    // on hello — reflect a board-side switch in the selector.
    const unlistenVoiceSource = listen<string>("voice-source", (event) => {
      const src = event.payload === "cloud" ? "cloud" : "system";
      setVoiceSource(src);
    });

    // The board reports WiFi provisioning progress.
    const unlistenWifi = listen<{ state: string; ssid: string }>("wifi-status", (event) => {
      const { state, ssid } = event.payload;
      setWifiStatus(ssid ? `${state} · ${ssid}` : state);
    });

    // The board reports device-license provisioning result.
    const unlistenLicense = listen<string>("license-status", (event) => {
      setLicStatus(event.payload);
    });

    const unlistenLicenseInfo = listen<LicenseInfo>("license-info", (event) => {
      setLicInfo(event.payload);
      setLicStatus(`read · ${event.payload.source}`);
    });

    // The AK reports authoritative values after hello and every local/remote change.
    const unlistenDeviceSettings = listen<DeviceSettings>("device-settings", (event) => {
      setDeviceSettings(event.payload);
      setStatus((current) => ({ ...current, device_settings_synced: true }));
    });

    // Recover the sink token/running state on mount (survives a UI reload).
    invoke<string>("status_token")
      .then((token) => {
        if (token) {
          setSsToken(token);
          setSsRunning(true);
        }
      })
      .catch(() => undefined);

    // Load persisted A/B/C chat numbers and optional AK display aliases.
    invoke<ChatNumbers>("kc_bind_get")
      .then(setBinds)
      .catch(() => undefined);
    invoke<ChatAliases>("kc_bind_aliases_get")
      .then(setBindAliases)
      .catch(() => undefined);
    invoke<ChatSessions>("kc_bind_sessions_get")
      .then(setBindSessions)
      .catch(() => undefined);

    // Recover the pinned "watched chat" on mount (survives a UI reload).
    invoke<string>("kc_get_pinned")
      .then((sid) => setPinnedSession(sid || ""))
      .catch(() => undefined);

    // Load the user-level global Crew hook status on first mount.
    invoke<HookStatus>("kc_hook_status", { exe: null })
      .then(setHookStatus)
      .catch(() => undefined);

    return () => {
      window.clearInterval(timer);
      unlistenLog.then((fn) => fn());
      unlistenBoard.then((fn) => fn());
      unlistenStatus.then((fn) => fn());
      unlistenVoiceSource.then((fn) => fn());
      unlistenWifi.then((fn) => fn());
      unlistenLicense.then((fn) => fn());
      unlistenLicenseInfo.then((fn) => fn());
      unlistenDeviceSettings.then((fn) => fn());
    };
  }, []);

  return (
    <main className="shell">
      <aside className="sidebar">
        <div className="brand">
          <h1>Agentic Keyboard</h1>
          <p>Companion · T5AI-Core</p>
        </div>
        {(["device", "input", "kirocrew", "logs"] as const).map((view) => (
          <button
            key={view}
            className={activeView === view ? "nav active" : "nav"}
            onClick={() => setActiveView(view)}
          >
            {view === "kirocrew" ? "LOCAL CTRL" : view.toUpperCase()}
          </button>
        ))}
        <div className="sidebar-footer">
          <p>Protocol v1 · UART 115200</p>
          <p>v0.1.0</p>
        </div>
      </aside>

      <section className="content">
        <header className="topbar">
          <div>
            <div className="eyebrow">UART / USB-CDC · 5 keys + 5-way nav · voice</div>
            <h2>{viewTitle(activeView)}</h2>
          </div>
          <div className={status.device_connected ? "status online" : "status"}>
            {status.device_connected ? "BOARD ONLINE" : "BOARD OFFLINE"}
          </div>
        </header>

        <section className="lcd-grid">
          <div className="lcd">
            <span>LINK</span>
            <strong>{status.device_connected ? status.transport.toUpperCase() : "OFFLINE"}</strong>
          </div>
          <div className="lcd">
            <span>ENDPOINT</span>
            <strong>{status.endpoint || "--"}</strong>
          </div>
          <div className="lcd">
            <span>FIRMWARE</span>
            <strong>{status.firmware_version || "--"}</strong>
          </div>
          <div className="lcd">
            <span>VOICE</span>
            <strong>{voiceActive ? "LISTENING" : "IDLE"}</strong>
          </div>
        </section>

        {(uiError || status.last_error) && (
          <div className="error-banner">{uiError || status.last_error}</div>
        )}

        {activeView === "device" && (
          <Panel title="Device Bridge">
            <label>Transport</label>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                className={transportMode === "usb" ? "" : "secondary"}
                onClick={() => setTransportMode("usb")}
              >
                USB
              </button>
              <button
                className={transportMode === "ble" ? "" : "secondary"}
                onClick={() => setTransportMode("ble")}
              >
                BLE
              </button>
            </div>

            {transportMode === "usb" ? (
              <>
                <label>Board USB port</label>
                <select value={selectedPort} onChange={(event) => setSelectedPort(event.target.value)}>
                  <option value="">Auto-detect (CH340/CH343)</option>
                  {boardPorts.map((port) => (
                    <option key={port.name} value={port.name}>
                      {port.name} · {port.kind}
                    </option>
                  ))}
                </select>
                <p className="hint good-text">
                  USB/UART is physically trusted — no pairing. The companion greets the board on connect.
                </p>
              </>
            ) : (
              <>
                <label>AK-KB device</label>
                <select value={selectedBle} onChange={(event) => setSelectedBle(event.target.value)}>
                  {bleDevices.length === 0 ? (
                    <option value="">No devices — press Scan</option>
                  ) : (
                    bleDevices.map((dev) => (
                      <option key={dev.id} value={dev.id}>
                        {dev.name || "AK-KB"} · {dev.id}
                      </option>
                    ))
                  )}
                </select>
                <div className="actions" style={{ marginTop: 6 }}>
                  <button
                    className="secondary"
                    disabled={bleScanning}
                    onClick={() => runAction(scanBle)}
                  >
                    {bleScanning ? "Scanning…" : "Scan"}
                  </button>
                </div>
                <p className="hint warn-text">
                  BLE GATT · current firmware exposes Tuya command service{" "}
                  <code>0xFD50</code> (notify <code>0x0002</code> board→PC, write
                  <code> 0x0001</code> PC→board); older builds use{" "}
                  <code>0x1910</code>/<code>0x2B10</code>/<code>0x2B11</code> and the
                  companion accepts either. Scan (~4s) finds boards advertising{" "}
                  <code>AK-KB</code>; the companion greets only after notify
                  subscription is live.
                </p>
              </>
            )}

            <div className="actions">
              <button className="secondary" onClick={() => runAction(connect)}>Connect</button>
              <button className="secondary" onClick={() => runAction(disconnect)}>Disconnect</button>
              <button className="secondary" onClick={() => runAction(refreshRuntime)}>Refresh</button>
            </div>

            <div className="device-settings-compact" aria-label="AK device settings">
              <span className="device-settings-title">DEVICE SETTINGS</span>
              {([
                { key: "volume" as const, label: "VOL", minimum: 0 },
                { key: "brightness" as const, label: "SCREEN", minimum: 10 },
              ]).map(({ key, label, minimum }) => (
                <div className="device-setting-control" key={key}>
                  <span>{label}</span>
                  <button
                    className="secondary"
                    aria-label={`Decrease ${label.toLowerCase()}`}
                    disabled={
                      !status.device_connected ||
                      !status.device_settings_synced ||
                      deviceSettingsBusy ||
                      deviceSettings[key] <= minimum
                    }
                    onClick={() => runAction(() => adjustDeviceSetting(key, -10))}
                  >
                    −
                  </button>
                  <strong>{deviceSettings[key]}%</strong>
                  <button
                    className="secondary"
                    aria-label={`Increase ${label.toLowerCase()}`}
                    disabled={
                      !status.device_connected ||
                      !status.device_settings_synced ||
                      deviceSettingsBusy ||
                      deviceSettings[key] >= 100
                    }
                    onClick={() => runAction(() => adjustDeviceSetting(key, 10))}
                  >
                    +
                  </button>
                </div>
              ))}
              <span className={`device-settings-sync ${status.device_settings_synced ? "good-text" : ""}`}>
                {!status.device_connected
                  ? "AK OFFLINE"
                  : status.device_settings_synced
                    ? "AK SYNCED"
                    : "WAITING FOR AK"}
              </span>
            </div>

            <label>Voice recognition path</label>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                className={voiceSource === "cloud" ? "" : "secondary"}
                onClick={() => runAction(() => selectVoiceSource("cloud"))}
              >
                Board cloud ASR
              </button>
              <button
                className={voiceSource === "system" ? "" : "secondary"}
                onClick={() => runAction(() => selectVoiceSource("system"))}
              >
                PC system ASR
              </button>
            </div>
            <p className="hint">
              <strong>Board cloud ASR</strong>: the board mic feeds Tuya cloud ASR; recognized text
              returns as an <code>asr</code> frame. <strong>PC system ASR</strong>: a K4 press sends
              <code> voice.start</code> and the companion activates Windows dictation (Win+H). The
              choice is mirrored to the board (and the board can switch it too — this reflects the
              board's reported path).
            </p>

            <label style={{ marginTop: 14 }}>WiFi provisioning (for board cloud ASR)</label>
            <input
              value={wifiSsid}
              disabled={!usbProvisioningAvailable || wifiBusy}
              onChange={(event) => setWifiSsid(event.target.value)}
              placeholder="WiFi SSID (2.4GHz)"
            />
            <div className="input-with-toggle" style={{ display: "flex", gap: 6, marginTop: 6 }}>
              <input
                type={showWifiPass ? "text" : "password"}
                value={wifiPass}
                disabled={!usbProvisioningAvailable || wifiBusy}
                onChange={(event) => setWifiPass(event.target.value)}
                placeholder="WiFi password"
                style={{ flex: 1 }}
                onKeyDown={(event) => {
                  if (event.key === "Enter" && usbProvisioningAvailable && !wifiBusy) {
                    runAction(sendWifi);
                  }
                }}
              />
              <button
                type="button"
                className="secondary"
                onClick={() => setShowWifiPass((v) => !v)}
                aria-label={showWifiPass ? "Hide password" : "Show password"}
                title={showWifiPass ? "Hide password" : "Show password"}
              >
                {showWifiPass ? "Hide" : "Show"}
              </button>
            </div>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                disabled={!usbProvisioningAvailable || wifiBusy}
                onClick={() => runAction(sendWifi)}
              >
                Configure WiFi
              </button>
            </div>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>WIFI</span>
              <strong className={wifiStatus.startsWith("connected") ? "good-text" : ""}>
                {wifiStatus || "-- (not configured)"}
              </strong>
            </div>
            <p className={`hint ${usbProvisioningAvailable ? "good-text" : "warn-text"}`}>
              USB connection required. WiFi credentials cannot be sent over BLE. The board joins
              its 2.4GHz AP using the cloud-ASR firmware profile.
            </p>

            <label style={{ marginTop: 14 }}>Re-enter pairing mode</label>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                className="secondary"
                disabled={!usbProvisioningAvailable || pairingBusy}
                onClick={() => enterPairing()}
              >
                Enter pairing mode
              </button>
            </div>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>PAIRING</span>
              <strong>{pairingStatus || "-- (idle)"}</strong>
            </div>
            <p className={`hint ${usbProvisioningAvailable ? "good-text" : "warn-text"}`}>
              USB connection required. Clears the board's saved WiFi and cloud binding and reboots
              it into provisioning so you can re-provision WiFi. The device license (UUID/AuthKey)
              is preserved. Same effect as three fast power cycles.
            </p>

            <label style={{ marginTop: 14 }}>Device license (UUID / AuthKey)</label>
            <input
              value={licUuid}
              disabled={!usbProvisioningAvailable || licBusy}
              onChange={(event) => setLicUuid(event.target.value)}
              placeholder="UUID (20 chars)"
            />
            <input
              type="password"
              value={licAuthkey}
              disabled={!usbProvisioningAvailable || licBusy}
              onChange={(event) => setLicAuthkey(event.target.value)}
              placeholder="AuthKey (32 chars)"
              style={{ marginTop: 6 }}
            />
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                disabled={!usbProvisioningAvailable || licBusy}
                onClick={() => runAction(sendLicense)}
              >
                Write license
              </button>
              <button
                className="secondary"
                disabled={!usbProvisioningAvailable || licBusy}
                onClick={() => runAction(readLicense)}
              >
                Read license info
              </button>
            </div>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>LICENSE</span>
              <strong className={licStatus.startsWith("written") ? "good-text" : ""}>
                {licStatus || "-- (not written)"}
              </strong>
            </div>
            {licInfo && (
              <p className="hint">
                Source: <code>{licInfo.source}</code> · UUID: <code>{licInfo.uuid || "--"}</code> ·
                AuthKey: <code>{licInfo.authkey_tail ? `****${licInfo.authkey_tail}` : "--"}</code>
              </p>
            )}
            <p className={`hint ${usbProvisioningAvailable ? "good-text" : "warn-text"}`}>
              USB connection required. UUID/AuthKey cannot be written over BLE. The license is
              stored on the board; the PID remains compile-time. Debug read shows the full UUID
              and only the last four AuthKey characters; Companion never persists the secret.
            </p>
          </Panel>
        )}

        {activeView === "input" && (
          <Panel title="Live Input">
            <p className="hint">
              Highlights the last key / direction reported by the board. Default intents shown are a
              placeholder for the remapping UI.
            </p>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>KIRO CREW CONTROL</span>
              <strong className={status.control_enabled ? "good-text" : ""}>
                {status.control_enabled ? "ENABLED — keys drive Kiro Crew" : "DISABLED — trigger check only"}
              </strong>
            </div>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                className={status.control_enabled ? "secondary" : ""}
                onClick={() => runAction(() => toggleControl(false))}
              >
                Disable control
              </button>
              <button
                className={status.control_enabled ? "" : "secondary"}
                onClick={() => runAction(() => toggleControl(true))}
              >
                Enable control
              </button>
            </div>
            <p className="hint">
              Default is <strong>enabled</strong>: board keys/nav drive the local Kiro Crew window,
              and K4 may activate system dictation. Disable control to keep input visualization
              while blocking every board-triggered PC action.
            </p>
            <div className="input-row">
              <div className="nav-cluster">
                <label className="cluster-label">5-WAY NAV</label>
                <div className="nav-pad">
                  {NAV_LAYOUT.map((dir, index) => (
                    <div
                      key={index}
                      className={
                        dir === null
                          ? "nav-cell"
                          : lastNav === dir
                            ? "nav-cell dir hit"
                            : "nav-cell dir"
                      }
                    >
                      {dir ? dir.toUpperCase() : ""}
                    </div>
                  ))}
                </div>
              </div>

              <div className="key-row">
                {KEY_MAP.map((key) => (
                  <div
                    className={
                      (lastKey === key.id ? "key-card hit" : "key-card") +
                      (key.wide ? " wide" : "")
                    }
                    key={key.id}
                  >
                    <div className="key-screen">{key.label}</div>
                    <div className="key-map">{key.intent}</div>
                  </div>
                ))}
              </div>
            </div>
          </Panel>
        )}

        {activeView === "kirocrew" && (
          <>
            <Panel title="Bind Chats with Keys (Control)">
              <p className="hint">
                Bind Chat A, B, and C to Kiro Crew chat shortcuts and optionally name each slot.
                Names are saved when you press Enter or click elsewhere, then shown on the AK;
                leave a name blank to display its default <strong>A, B, or C</strong>. Use up to
                {ALIAS_MAX_CHARS} English letters, numbers, spaces, or symbols.
              </p>

              {[1, 2, 3].map((slot) => {
                const n = binds[slot - 1];
                const alias = bindAliases[slot - 1] ?? "";
                const defaultName = SLOT_DEFAULT_NAMES[slot - 1];
                return (
                  <div key={slot} className="binding-row">
                    <strong className="binding-slot-label">
                      {`Chat ${defaultName} (K${slot}):`}
                    </strong>
                    <input
                      className="binding-alias-input"
                      value={alias}
                      maxLength={ALIAS_MAX_CHARS}
                      disabled={bindBusy}
                      aria-label={`Chat ${defaultName} name; blank uses ${defaultName}`}
                      placeholder={`Name · default ${defaultName}`}
                      onChange={(event) => bindAliasDraft(slot, event.target.value)}
                      onBlur={(event) =>
                        runBind(() => bindAliasSave(slot, event.currentTarget.value))
                      }
                      onKeyDown={(event) => {
                        if (event.key === "Enter") event.currentTarget.blur();
                      }}
                    />
                    <span className="binding-alt-label">Alt+</span>
                    <select
                      className="binding-chat-select"
                      value={n ?? ""}
                      disabled={bindBusy}
                      aria-label={`Chat ${defaultName} shortcut number`}
                      onChange={(event) =>
                        runBind(() => bindSet(slot, Number(event.target.value)))
                      }
                    >
                      {[1, 2, 3, 4, 5, 6, 7, 8, 9].map((num) => (
                        <option key={num} value={num}>
                          {num}
                        </option>
                      ))}
                    </select>
                    <button
                      className="secondary binding-reset"
                      disabled={bindBusy}
                      onClick={() => runBind(() => bindClear(slot))}
                    >
                      Reset to {slot}
                    </button>
                  </div>
                );
              })}

              {bindError && <div className="error-banner" style={{ marginTop: 14 }}>{bindError}</div>}
            </Panel>

            <Panel title="Bind the Window">
              <p className="hint">
                Focus a <strong>local</strong> Kiro Crew window and inject keystrokes directly
                (no HTTP). Board keys route to command-bar / chords through the same backend.
              </p>

              <label>Window title match</label>
              <input
                value={lcTitle}
                onChange={(event) => setLcTitle(event.target.value)}
                placeholder="Kiro Crew (blank = try Kiro Crew / Agentic)"
              />
              <div className="actions" style={{ marginTop: 6 }}>
                <button disabled={lcBusy} onClick={() => runLc(lcBind)}>
                  Bind window
                </button>
              </div>

              <div className="lcd" style={{ marginTop: 14 }}>
                <span>FOCUS</span>
                <strong>
                  {lcBound ? "BOUND" : "-- (bind first)"}
                  {lcResult ? ` · ${lcResult}` : ""}
                </strong>
              </div>

              <label style={{ marginTop: 18 }}>Text / command</label>
              <input
                value={lcText}
                onChange={(event) => setLcText(event.target.value)}
                placeholder="Type text to inject, or a command-bar command…"
                onKeyDown={(event) => {
                  if (event.key === "Enter" && !lcBusy) runLc(lcCommandBar);
                }}
              />
              <div className="actions" style={{ marginTop: 6 }}>
                <button disabled={lcBusy} onClick={() => runLc(lcSendText)}>
                  Send text
                </button>
                <button disabled={lcBusy} onClick={() => runLc(lcCommandBar)}>
                  Command bar (Ctrl+K)
                </button>
              </div>

              <label style={{ marginTop: 18 }}>Quick chords</label>
              <div className="actions" style={{ marginTop: 6, flexWrap: "wrap" }}>
                {["enter", "esc", "up", "down", "left", "right"].map((name) => (
                  <button
                    key={name}
                    className="secondary"
                    disabled={lcBusy}
                    onClick={() => runLc(() => lcChord(name))}
                  >
                    {name.toUpperCase()}
                  </button>
                ))}
              </div>

              {lcError && <div className="error-banner" style={{ marginTop: 14 }}>{lcError}</div>}
            </Panel>

            <Panel title="Bind Chats with Agent Status (Local Hook Listener)">
              <p className="hint">
                Receives Kiro Crew <strong>agent hooks</strong> on
                <code> 127.0.0.1:5477/status</code> and mirrors each agent's state to the board
                LCD as a <code>display</code> frame. The hook is a mode of this very binary
                (<code>companion --hook</code>), gated by a per-run token.
              </p>

              <label style={{ marginTop: 8 }}>Global Crew hook installation</label>
              <p className="hint" style={{ marginTop: 4 }}>
                Adds four global entries to Crew's shared ScriptHookStore at
                <code>{hookStatus?.hooks_dir ?? "~/.kiro/crew"}</code>. It covers
                <em> AgentSpawn / UserPromptSubmit / PostToolUse / Stop</em> and never
                modifies the separate ACP Agent Hooks shown below it in Crew.
              </p>
              <div className="lcd" style={{ marginTop: 6 }}>
                <span>HOOK</span>
                <strong className={hookStatus?.fully_installed ? "good-text" : ""}>
                  {!hookStatus
                    ? "-- (checking)"
                    : hookStatus.fully_installed
                    ? "CONFIGURED · 4/4"
                    : hookStatus.any_installed
                    ? `PARTIAL · ${hookStatus.installed_events}/4`
                    : "NOT INSTALLED"}
                </strong>
              </div>
              <p className="hint" style={{ marginTop: 4 }}>
                <code>{hookStatus?.hook_file ?? "~/.kiro/crew/hooks.json"}</code>
              </p>
              <div className="actions" style={{ marginTop: 6 }}>
                <button className="secondary" disabled={ssBusy} onClick={() => runSs(hookRefresh)}>
                  Refresh
                </button>
                <button className="secondary" disabled={ssBusy} onClick={() => runSs(hookOpenFolder)}>
                  Open Crew folder
                </button>
                <button disabled={ssBusy} onClick={() => runSs(hookInstall)}>
                  Install globally
                </button>
                <button
                  className="secondary"
                  disabled={ssBusy || !hookStatus?.any_installed}
                  onClick={() => runSs(hookRemove)}
                >
                  Remove global hook
                </button>
              </div>
              <p className="hint" style={{ marginTop: 4, opacity: 0.7 }}>
                After install, reinstall, or removal, click Apply & Restart in Kiro Crew (or
                restart Crew) so its in-memory HookStore reloads hooks.json. Reinstalling is
                idempotent and refreshes the Companion executable path.
              </p>

              <div className="listener-control-row">
                <button
                  type="button"
                  role="switch"
                  aria-checked={ssRunning}
                  aria-label={ssRunning ? "Stop status listener" : "Start status listener"}
                  className="listener-switch"
                  disabled={ssBusy}
                  onClick={() => runSs(ssRunning ? ssStop : ssStart)}
                >
                  <span className="listener-switch-action">{ssRunning ? "Stop" : "Start"}</span>
                  <span className="listener-switch-track" aria-hidden="true">
                    <span className="listener-switch-thumb" />
                  </span>
                </button>
                <div className="listener-inline-status" aria-live="polite">
                  <span>LISTENER</span>
                  <strong className={ssRunning ? "good-text" : ""}>
                    {ssRunning ? "LISTENING · 127.0.0.1:5477" : "STOPPED"}
                  </strong>
                </div>
              </div>

              {ssRunning && (
                <details className="listener-details">
                  <summary>
                    Listener details <span>Token &amp; global hook command</span>
                  </summary>
                  <div className="listener-details-body">
                    <label>Token (X-AK-Status-Token)</label>
                    <input readOnly value={ssToken} onFocus={(e) => e.currentTarget.select()} />

                    <label>Global hook command</label>
                    <div className="terminal" style={{ whiteSpace: "pre-wrap" }}>
                      {hookStatus?.command ?? '"<path-to-companion.exe>" --hook'}
                    </div>
                    <p className="hint">
                      Each Crew ScriptHook invokes this command for dashboard agent turns. The hook
                      reads the sink port and token from
                      <code> %APPDATA%\ak-companion\hook.json</code> (Windows) /
                      <code> ~/.config/ak-companion/hook.json</code> (else). Starting the sink writes
                      that handoff file; no token is stored in the Crew hook configuration.
                    </p>
                  </div>
                </details>
              )}

              <div className="watched-toolbar">
                <div>
                  <label>Watched chat (shown on the keyboard screen)</label>
                  <p className="hint" style={{ marginTop: 4 }}>
                    {pinnedSession
                      ? `Watching ${pinnedSession} — only this chat appears on the keyboard screen; other chats are hidden.`
                      : "Following all sessions — the latest active chat updates the keyboard screen."}
                  </p>
                </div>
                <button
                  className="secondary"
                  disabled={!pinnedSession || ssBusy}
                  onClick={() => runSs(unpinSession)}
                >
                  Follow all
                </button>
              </div>

              <p className="hint watched-model-hint">
                Chat A–C send a Kiro Crew shortcut (Alt+1–9) for the selected Chat #.
                Watch chooses which live chat can update the keyboard screen.
              </p>

              <div className="watched-tabs" role="tablist" aria-label="Watched chat view">
                <button
                  role="tab"
                  aria-selected={watchedView === "compact"}
                  className={watchedView === "compact" ? "watched-tab active" : "watched-tab"}
                  onClick={() => setWatchedView("compact")}
                >
                  Compact
                </button>
                <button
                  role="tab"
                  aria-selected={watchedView === "detail"}
                  className={watchedView === "detail" ? "watched-tab active" : "watched-tab"}
                  onClick={() => setWatchedView("detail")}
                >
                  Detail
                </button>
              </div>

              <div className="watched-list">
                {sortedKcStatuses.length === 0 ? (
                  <div className="watched-empty">
                    No active chats yet. Start the status listener above, then run a Kiro Crew turn.
                  </div>
                ) : watchedView === "compact" ? (
                  sortedKcStatuses.map((s) => {
                    const isPinned = s.session_id === pinnedSession;
                    const boundSlot = bindSessions.findIndex((id) => id === s.session_id) + 1;
                    const draftForSession = quickBindDraft?.sessionId === s.session_id;
                    return (
                      <div
                        className={`watched-compact-card${isPinned ? " pinned" : ""}`}
                        key={s.session_id}
                      >
                        <div className="watched-compact-row">
                          <div className="watched-session-summary">
                            <strong>{sessionDisplayName(s.session_id)}</strong>
                            <code>{s.session_id}</code>
                          </div>
                          <span className={`watched-state state-${s.state.toLowerCase()}`}>
                            {s.state.toUpperCase()}
                          </span>
                          <div className="watched-slot-binding">
                            <span>Bind chat</span>
                            <div className="watched-slot-actions" aria-label="Quick bind keys">
                              {[1, 2, 3].map((slot) => (
                              <button
                                key={slot}
                                className={bindSessions[slot - 1] === s.session_id ? "bound" : "secondary"}
                                disabled={bindBusy}
                                aria-label={`Bind ${s.session_id} to Chat ${SLOT_DEFAULT_NAMES[slot - 1]}`}
                                onClick={() => startQuickBind(s.session_id, slot)}
                              >
                                {SLOT_DEFAULT_NAMES[slot - 1]}
                              </button>
                              ))}
                            </div>
                          </div>
                          <button
                            className={isPinned ? "watched-watch active" : "watched-watch secondary"}
                            disabled={ssBusy}
                            onClick={() =>
                              runSs(() => isPinned ? unpinSession() : pinSession(s.session_id))
                            }
                          >
                            {isPinned ? "Watch all" : "Watch"}
                          </button>
                        </div>
                        <div className="watched-compact-meta">
                          {boundSlot
                            ? `Chat ${SLOT_DEFAULT_NAMES[boundSlot - 1]} → Kiro Crew Chat #${binds[boundSlot - 1]} (Alt+${binds[boundSlot - 1]}) · screen label ${bindAliases[boundSlot - 1] || SLOT_DEFAULT_NAMES[boundSlot - 1]}`
                            : s.text}
                        </div>
                        {draftForSession && quickBindDraft && (
                          <div className="quick-bind-editor">
                            <strong>Bind Chat {SLOT_DEFAULT_NAMES[quickBindDraft.slot - 1]}</strong>
                            <label htmlFor={`quick-chat-${quickBindDraft.slot}`}>Kiro Crew Chat #</label>
                            <select
                              id={`quick-chat-${quickBindDraft.slot}`}
                              value={quickBindDraft.number}
                              disabled={bindBusy}
                              onChange={(event) =>
                                setQuickBindDraft({
                                  ...quickBindDraft,
                                  number: Number(event.target.value),
                                })
                              }
                            >
                              {[1, 2, 3, 4, 5, 6, 7, 8, 9].map((number) => (
                                <option key={number} value={number}>{number}</option>
                              ))}
                            </select>
                            <button disabled={bindBusy} onClick={confirmQuickBind}>Save binding</button>
                            {bindSessions[quickBindDraft.slot - 1] && (
                              <button
                                className="secondary"
                                disabled={bindBusy}
                                onClick={() => runBind(() => clearBoundSession(quickBindDraft.slot))}
                              >
                                Remove session link
                              </button>
                            )}
                            <button
                              className="secondary"
                              disabled={bindBusy}
                              onClick={() => setQuickBindDraft(null)}
                            >
                              Cancel
                            </button>
                          </div>
                        )}
                      </div>
                    );
                  })
                ) : (
                  sortedKcStatuses.map((s) => {
                    const isPinned = s.session_id === pinnedSession;
                    const driving = pinnedSession ? isPinned : true;
                    return (
                      <article
                        className={`watched-detail-card${isPinned ? " pinned" : ""}${driving ? "" : " muted"}`}
                        key={s.session_id}
                      >
                        <div className="watched-detail-head">
                          <div>
                            <strong>{sessionDisplayName(s.session_id)}</strong>
                            <span>{s.agent_name}</span>
                          </div>
                          <span className={`watched-state state-${s.state.toLowerCase()}`}>
                            {s.state.toUpperCase()}
                          </span>
                          <button
                            className={isPinned ? "watched-watch active" : "watched-watch secondary"}
                            disabled={ssBusy}
                            onClick={() =>
                              runSs(() => isPinned ? unpinSession() : pinSession(s.session_id))
                            }
                          >
                            {isPinned ? "Watch all" : "Watch"}
                          </button>
                        </div>
                        <dl className="watched-detail-fields">
                          <div><dt>Session ID</dt><dd><code>{s.session_id}</code></dd></div>
                          <div><dt>Event</dt><dd>{s.event || "Status update"}</dd></div>
                          <div><dt>Keyboard</dt><dd>{driving ? "Shown on keyboard screen" : "Hidden while another chat is watched"}</dd></div>
                        </dl>
                        <div className="watched-detail-text">{s.detail_text ?? s.text}</div>
                      </article>
                    );
                  })
                )}
              </div>

              {bindError && <div className="error-banner" style={{ marginTop: 14 }}>{bindError}</div>}
              {ssError && <div className="error-banner" style={{ marginTop: 14 }}>{ssError}</div>}
            </Panel>
          </>
        )}

        {activeView === "logs" && (
          <Panel title="Event Stream">
            <div className="actions" style={{ marginTop: 0 }}>
              <button
                className={logsShowRaw ? "secondary" : ""}
                onClick={() => setLogsShowRaw(false)}
              >
                Detailed
              </button>
              <button
                className={logsShowRaw ? "" : "secondary"}
                onClick={() => setLogsShowRaw(true)}
              >
                Raw JSON
              </button>
              <button
                className={logsPaused ? "" : "secondary"}
                onClick={toggleLogsPause}
                title={logsPaused ? "继续滚动并补回暂停期间的日志" : "冻结日志便于观察/复制"}
              >
                {logsPaused ? `▶ 继续${pausedCount ? ` (+${pausedCount})` : ""}` : "⏸ 暂停"}
              </button>
              <button className="secondary" onClick={() => runAction(clearLogs)}>Clear</button>
            </div>
            <p className="hint">
              {logsPaused
                ? `已暂停 — 显示已冻结，可安心选择/复制${
                    pausedCount ? `；暂停期间缓冲了 ${pausedCount} 条新日志，点"继续"补回` : ""
                  }。`
                : logsShowRaw
                ? "Raw board-frame JSON, one object per line (as received over the transport)."
                : "Human-readable event log ([RX] key/nav/voice, [KC-CTL], [INFO]/[ERROR])."}
            </p>
            <div className="log">
              {logsShowRaw ? (
                logsRaw.length === 0
                  ? "No frames yet. Connect the board to see raw JSON frames."
                  : logsRaw.map((line, index) => <div key={`raw-${index}`}>{line}</div>)
              ) : logs.length === 0 ? (
                "No events yet. Connect the board to see key / nav / voice frames."
              ) : (
                logs.map((line, index) => <div key={`${index}-${line}`}>{line}</div>)
              )}
            </div>
          </Panel>
        )}
      </section>
    </main>
  );
}

function Panel({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="panel">
      <h3>{title}</h3>
      {children}
    </section>
  );
}

function viewTitle(view: string): string {
  switch (view) {
    case "input":
      return "Live Input";
    case "kirocrew":
      return "Local Control";
    case "logs":
      return "Event Stream";
    default:
      return "Device Control";
  }
}

createRoot(document.getElementById("root")!).render(<App />);
