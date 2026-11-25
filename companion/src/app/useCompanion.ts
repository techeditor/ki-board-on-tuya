/* Companion state and backend bridge for the redesign shell. Behaviour (invoke
 * names, events, validation, long-press bind, log pause buffering) is ported
 * from the previous entry (src/main.tsx); the layout is not. Views take the
 * object this hook returns and never call invoke themselves. */
import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { useI18n, isLocale, type Locale } from "../i18n";
import { useBoardStatus } from "../ui/boardStatus";
import {
  clearLiveKeys,
  expire,
  initialLiveKeys,
  nextExpiry,
  onBoardEvent,
  type BoardKeyEvent,
  type LiveKeysState,
} from "./liveKeys";
import { toBindings } from "../keymap/actions";
import type { KeymapState } from "../ui/KeymapPanel";
import {
  ALIAS_MAX_CHARS,
  defaultStatus,
  isIdentityFailure,
  type ActivationStatus,
  type BleDeviceInfo,
  type ChatAliases,
  type ChatNumbers,
  type ChatSessions,
  type CrewConnection,
  type CrewSlot,
  type DeviceSettings,
  type HookStatus,
  type KcStatus,
  type LicenseInfo,
  type PidStatus,
  type QuickBindDraft,
  type SerialPortInfo,
  type Status,
  type WatchedView,
  buildQrDrawing,
  activationStepIndex,
  type QrDrawing,
  type ConnPrefs,
  defaultConnPrefs,
} from "./companionTypes";

export type Model = ReturnType<typeof useCompanion>;

/** True only inside the Tauri webview; the browser preview runs without a backend. */
export function tauriReady(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

function errText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export function useCompanion() {
  const { t, locale, setLocale } = useI18n();
  // Listeners are registered once; they read the latest t() through this ref.
  const tRef = useRef(t);
  tRef.current = t;

  const [status, setStatus] = useState<Status>(defaultStatus);
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [connPrefs, setConnPrefs] = useState<ConnPrefs>(defaultConnPrefs);
  const [bleDevices, setBleDevices] = useState<BleDeviceInfo[]>([]);
  const [bleScanning, setBleScanning] = useState(false);

  const [logs, setLogs] = useState<string[]>([]);
  const [logsRaw, setLogsRaw] = useState<string[]>([]);
  const [logsPaused, setLogsPaused] = useState(false);
  const [pausedCount, setPausedCount] = useState(0);
  const logsPausedRef = useRef(false);
  const logBufRef = useRef<string[]>([]);
  const rawBufRef = useRef<string[]>([]);

  const [voiceSource, setVoiceSource] = useState<"cloud" | "system">("cloud");
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [wifiStatus, setWifiStatus] = useState("");
  const [wifiBusy, setWifiBusy] = useState(false);
  const [activationStatus, setActivationStatus] = useState<ActivationStatus>({ state: "unknown", qr_url: "" });
  const [activationExpanded, setActivationExpanded] = useState(false);
  const [licUuid, setLicUuid] = useState("");
  const [licAuthkey, setLicAuthkey] = useState("");
  const [licStatus, setLicStatus] = useState("");
  const [licBusy, setLicBusy] = useState(false);
  const [licInfo, setLicInfo] = useState<LicenseInfo | null>(null);
  const [pidInput, setPidInput] = useState("");
  const [pidStatus, setPidStatus] = useState("");
  const [pidBusy, setPidBusy] = useState(false);
  const [pairingBusy, setPairingBusy] = useState(false);
  const [pairingStatus, setPairingStatus] = useState("");
  const [deviceSettings, setDeviceSettings] = useState<DeviceSettings>({
    volume: defaultStatus.device_volume,
    brightness: defaultStatus.device_brightness,
  });
  const [deviceSettingsBusy, setDeviceSettingsBusy] = useState(false);
  const [live, setLive] = useState<LiveKeysState>(initialLiveKeys);
  const [voiceActive, setVoiceActive] = useState(false);
  const [uiError, setUiError] = useState("");
  const statusPollInFlightRef = useRef(false);

  const board = useBoardStatus();
  const usbOk = status.device_connected && status.transport === "usb";
  const provWindowOpen = usbOk && board.provUntilMs > board.nowMs;

  // Live keys: drop pulses and the knob's burst label when their time is up,
  // and put every key out when the keyboard goes away mid-press.
  useEffect(() => {
    const at = nextExpiry(live);
    if (at === null) return;
    const timer = window.setTimeout(() => setLive((cur) => expire(cur, Date.now())), Math.max(0, at - Date.now()));
    return () => window.clearTimeout(timer);
  }, [live]);
  useEffect(() => {
    if (!status.device_connected) setLive(clearLiveKeys);
  }, [status.device_connected]);
  // Close the QR window (and the QR URL, which carries the UUID) when pairing ends.
  useEffect(() => {
    if (!provWindowOpen) {
      setActivationStatus((current) => (current.qr_url ? { ...current, qr_url: "" } : current));
    }
  }, [provWindowOpen]);

  // Show a keyboard refusal once per new event (not on locale change).
  useEffect(() => {
    const { seq, command, reason } = board.rejection;
    if (seq === 0) return;
    const tt = tRef.current;
    if (reason === "confirm_on_device") setPairingStatus(tt("conn.pairing.confirmOnDevice"));
    else if (reason === "closed") setUiError(tt("conn.error.windowClosed"));
    else if (reason === "exists") setUiError(tt("conn.error.exists"));
    else if (reason === "cancelled") setUiError(tt("conn.error.cancelled"));
    else setUiError(tt("conn.error.refused", { cmd: command, reason }));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [board.rejection.seq]);

  const activationQrDrawing = useMemo<QrDrawing | null>(
    () => (activationStatus.state === "scan_qr" ? buildQrDrawing(activationStatus.qr_url) : null),
    [activationStatus.state, activationStatus.qr_url],
  );
  const activationQrError =
    activationStatus.state === "scan_qr" && activationStatus.qr_url !== "" && activationQrDrawing === null
      ? t("act.badUrl")
      : "";

  // Legacy shortcut injection (window focus + keystrokes).
  const [lcTitle, setLcTitle] = useState("");
  const [lcBound, setLcBound] = useState(false);
  const [lcText, setLcText] = useState("");
  const [lcResult, setLcResult] = useState("");
  const [lcError, setLcError] = useState("");
  const [lcBusy, setLcBusy] = useState(false);

  // Primary Crew API (REST + WebSocket).
  const [crewSlots, setCrewSlots] = useState<CrewSlot[]>([]);
  const [crewSelectedSlot, setCrewSelectedSlot] = useState("");
  const [crewMessage, setCrewMessage] = useState("");
  const [crewApiBusy, setCrewApiBusy] = useState(false);
  const [crewApiError, setCrewApiError] = useState("");

  // A/B/C chat keys: stable API chat, Alt+N jump number and display alias.
  const [binds, setBinds] = useState<ChatNumbers>([]);
  const [bindAliases, setBindAliases] = useState<ChatAliases>(["", "", ""]);
  const [bindSessions, setBindSessions] = useState<ChatSessions>(["", "", ""]);
  const [bindError, setBindError] = useState("");
  const [bindBusy, setBindBusy] = useState(false);

  // Status sink (hook listener) and per-session status.
  const [ssRunning, setSsRunning] = useState(false);
  const [ssToken, setSsToken] = useState("");
  const [ssError, setSsError] = useState("");
  const [ssBusy, setSsBusy] = useState(false);
  const [kcStatuses, setKcStatuses] = useState<Record<string, KcStatus>>({});
  const [watchedView, setWatchedView] = useState<WatchedView>("compact");
  const [quickBindDraft, setQuickBindDraft] = useState<QuickBindDraft | null>(null);
  const [pinnedSession, setPinnedSession] = useState("");
  const [hookStatus, setHookStatus] = useState<HookStatus | null>(null);

  const boardPorts = useMemo(() => ports.filter((port) => port.kind !== "flash"), [ports]);
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
    // Merge: keep the board-reported language and PID state between device-settings events.
    setDeviceSettings((d) => ({ ...d, volume: runtime.device_volume, brightness: runtime.device_brightness }));
    setPorts(await invoke<SerialPortInfo[]>("list_serial_ports"));
    if (!logsPausedRef.current) setLogs(await invoke<string[]>("get_logs"));
  }

  async function runAction(action: () => Promise<void>) {
    try {
      setUiError("");
      await action();
    } catch (error) {
      setUiError(errText(error));
    }
  }

  async function connect() {
    setLicInfo(null);
    if (connPrefs.transport === "ble") {
      if (!connPrefs.ble_device) throw new Error(t("conn.transport.scanFirst"));
      await invoke("connect_device", { transport: "ble", portName: connPrefs.ble_device });
    } else {
      try {
        await invoke("connect_device", { transport: "usb", portName: connPrefs.port || null });
      } catch (error) {
        // Auto mode falls back to the saved BLE device when no USB port answers.
        if (connPrefs.transport !== "auto" || !connPrefs.ble_device) throw error;
        await invoke("connect_device", { transport: "ble", portName: connPrefs.ble_device });
      }
    }
    await refreshRuntime();
  }

  // Save the link choice (transport, port, BLE device, auto-connect) to connection.json.
  async function updateConnPrefs(patch: Partial<ConnPrefs>) {
    const next = { ...connPrefs, ...patch };
    setConnPrefs(next);
    await invoke("set_connection_prefs", { prefs: next });
  }

  async function scanBle() {
    setBleScanning(true);
    try {
      const found = await invoke<BleDeviceInfo[]>("scan_ble");
      setBleDevices(found);
      if (found.length > 0 && !found.some((d) => d.id === connPrefs.ble_device)) {
        await updateConnPrefs({ ble_device: found[0].id });
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

  async function adjustDeviceSetting(key: "volume" | "brightness", delta: number) {
    const minimum = key === "brightness" ? 10 : 0;
    const current = deviceSettings[key];
    const nextValue = Math.max(minimum, Math.min(100, current + delta));
    if (nextValue === current) return;
    setDeviceSettingsBusy(true);
    try {
      await invoke("set_device_settings", { volume: deviceSettings.volume, brightness: deviceSettings.brightness, [key]: nextValue });
    } finally {
      setDeviceSettingsBusy(false);
    }
  }

  // FR-01: one language for the Companion and the keyboard.
  function chooseLocale(next: Locale) {
    setLocale(next);
    if (status.device_settings_synced) {
      invoke("set_device_language", { language: next }).catch((e) => console.warn("set_device_language failed", e));
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

  // Pause freezes the visible list; resume merges what was buffered meanwhile.
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

  async function toggleControl(enabled: boolean) {
    await invoke("set_control_enabled", { enabled });
    setStatus((s) => ({ ...s, control_enabled: enabled }));
  }

  async function toggleFocusOnSelect(enabled: boolean) {
    await invoke("set_focus_on_select", { enabled });
    setStatus((s) => ({ ...s, focus_on_select: enabled }));
  }

  async function toggleShowWorkers(enabled: boolean) {
    await invoke("set_show_workers", { enabled });
    setStatus((s) => ({ ...s, show_workers: enabled }));
  }

  async function setVoiceForceKeyboard(enabled: boolean) {
    await invoke("set_voice_force_keyboard", { enabled });
    setStatus((s) => ({ ...s, voice_force_keyboard: enabled }));
  }

  async function selectVoiceSource(source: "cloud" | "system") {
    setVoiceSource(source);
    await invoke("set_voice_source", { source }).catch(() => undefined);
  }

  async function sendWifi() {
    if (!usbOk) throw new Error(t("conn.error.usbWifi"));
    if (!provWindowOpen) throw new Error(t("conn.error.windowClosed"));
    if (!wifiSsid.trim()) throw new Error(t("conn.error.ssid"));
    setWifiBusy(true);
    setActivationStatus({ state: "connecting_router", qr_url: "" });
    setActivationExpanded(false);
    try {
      setUiError("");
      setWifiStatus(t("conn.wifi.sending"));
      await invoke("send_wifi", { ssid: wifiSsid.trim(), pass: wifiPass });
      setWifiStatus(t("conn.wifi.sent"));
    } catch (error) {
      setWifiStatus("");
      throw error;
    } finally {
      setWifiBusy(false);
    }
  }

  async function sendLicense() {
    if (!usbOk) throw new Error(t("conn.error.usbLicense"));
    if (!provWindowOpen) throw new Error(t("conn.error.windowClosed"));
    if (!licUuid.trim() || !licAuthkey.trim()) throw new Error(t("conn.error.licRequired"));
    setLicBusy(true);
    try {
      setUiError("");
      setLicStatus(t("conn.lic.writing"));
      await invoke("send_license", { uuid: licUuid.trim(), authkey: licAuthkey.trim() });
      setLicStatus(t("conn.lic.sent"));
    } catch (error) {
      setLicStatus("");
      throw error;
    } finally {
      setLicBusy(false);
    }
  }

  // USB only; the keyboard accepts a PID once, while it has none, and applies it after a restart.
  async function sendPid() {
    if (!usbOk) throw new Error(t("conn.pid.usbOnly"));
    const pid = pidInput.trim();
    if (!/^[a-z0-9]{16}$/.test(pid)) throw new Error(t("conn.pid.invalid"));
    setPidBusy(true);
    try {
      setUiError("");
      setPidStatus(t("conn.lic.writing"));
      await invoke("send_pid", { pid });
      setPidInput("");
      setPidStatus(t("conn.lic.sent"));
    } catch (error) {
      setPidStatus("");
      throw error;
    } finally {
      setPidBusy(false);
    }
  }

  async function readLicense() {
    if (!usbOk) throw new Error(t("conn.error.usbDiag"));
    if (!provWindowOpen) throw new Error(t("conn.error.windowClosed"));
    setLicBusy(true);
    try {
      setUiError("");
      setLicStatus(t("conn.lic.reading"));
      setLicInfo(null);
      await invoke("read_license");
    } catch (error) {
      setLicStatus("");
      throw error;
    } finally {
      setLicBusy(false);
    }
  }

  // Destructive: clears the Tuya binding and Wi-Fi. Callers confirm first.
  async function enterPairing() {
    if (!usbOk) throw new Error(t("conn.error.usbPair"));
    if (!provWindowOpen) throw new Error(t("conn.error.windowClosed"));
    setPairingBusy(true);
    try {
      setUiError("");
      setPairingStatus(t("conn.lic.sending"));
      await invoke("enter_pairing");
      setPairingStatus(t("conn.pairing.confirmOnDevice"));
    } catch (error) {
      setPairingStatus("");
      throw error;
    } finally {
      setPairingBusy(false);
    }
  }

  async function runCrewApi<T>(action: () => Promise<T>): Promise<T | undefined> {
    setCrewApiBusy(true);
    try {
      setCrewApiError("");
      return await action();
    } catch (error) {
      setCrewApiError(errText(error));
      return undefined;
    } finally {
      setCrewApiBusy(false);
    }
  }

  async function refreshCrewSlots() {
    const slots = await invoke<CrewSlot[]>("crew_api_list_slots");
    setCrewSlots(slots);
    if (!crewSelectedSlot || !slots.some((slot) => slot.key === crewSelectedSlot)) {
      setCrewSelectedSlot(slots[0]?.key ?? "");
    }
  }

  async function sendCrewApiMessage() {
    const message = crewMessage.trim();
    if (!crewSelectedSlot) throw new Error(t("control.api.selectFirst"));
    if (!message) throw new Error(t("control.api.typeFirst"));
    await invoke("crew_api_send_message", { slot: crewSelectedSlot, message });
    setCrewMessage("");
  }

  async function crewSlotAction(action: "continue" | "interrupt" | "stop") {
    if (!crewSelectedSlot) throw new Error(t("control.api.selectFirst"));
    await invoke("crew_api_slot_action", { slot: crewSelectedSlot, action });
  }

  async function runLc(action: () => Promise<void>) {
    setLcBusy(true);
    try {
      setLcError("");
      await action();
    } catch (error) {
      setLcError(errText(error));
      setLcBound(false);
      setLcResult(t("control.legacy.failed"));
    } finally {
      setLcBusy(false);
    }
  }

  async function lcBind() {
    await invoke("kc_bind_window", { title: lcTitle.trim() });
    setLcBound(true);
    setLcResult(t("control.legacy.focused", { name: lcTitle.trim() || "Kiro Crew / Agentic" }));
  }

  async function lcSendText() {
    const text = lcText.trim();
    if (!text) throw new Error(t("control.legacy.typeText"));
    await invoke("kc_send_text", { text });
    setLcResult(t("control.legacy.sent", { n: text.length }));
  }

  async function lcCommandBar() {
    const cmd = lcText.trim();
    if (!cmd) throw new Error(t("control.legacy.typeCmd"));
    await invoke("kc_command_bar", { cmd });
    setLcResult(t("control.legacy.cmd", { cmd }));
    setLcText("");
  }

  async function lcChord(name: string) {
    await invoke("kc_chord", { name });
    setLcResult(t("control.legacy.chord", { name }));
  }

  async function runBind(action: () => Promise<void>) {
    setBindBusy(true);
    try {
      setBindError("");
      await action();
    } catch (error) {
      setBindError(errText(error));
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

  async function bindAliasSave(slot: number, alias: string) {
    const aliases = await invoke<ChatAliases>("kc_bind_alias_set", { slot, alias });
    setBindAliases(aliases);
  }

  function bindAliasDraft(slot: number, alias: string) {
    setBindAliases((current) => {
      const next = [...current];
      next[slot - 1] = alias;
      return next;
    });
  }

  async function runSs(action: () => Promise<void>) {
    setSsBusy(true);
    try {
      setSsError("");
      await action();
    } catch (error) {
      setSsError(errText(error));
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

  async function pinSession(sessionId: string) {
    await invoke("kc_pin_session", { sessionId });
    setPinnedSession(sessionId);
  }

  async function unpinSession() {
    await invoke("kc_unpin_session");
    setPinnedSession("");
  }

  async function hookRefresh() {
    setHookStatus(await invoke<HookStatus>("kc_hook_status"));
  }

  async function hookOpenFolder() {
    await invoke<string>("kc_open_hooks_folder");
  }

  async function hookInstall() {
    setHookStatus(await invoke<HookStatus>("kc_hook_install"));
  }

  async function hookRemove() {
    setHookStatus(await invoke<HookStatus>("kc_hook_remove"));
  }

  async function applyKeymap(state: KeymapState) {
    await invoke("set_keymap", { bindings: toBindings(state) });
  }

  // Backend bridge: poll status and subscribe to board/crew events. Skipped in
  // the browser preview, where no Tauri backend exists.
  useEffect(() => {
    if (!tauriReady()) return;
    refreshRuntime().catch(() => undefined);
    invoke<ConnPrefs>("get_connection_prefs")
      .then(setConnPrefs)
      .catch(() => undefined);
    const timer = window.setInterval(() => {
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

    const unlistenBoard = listen<BoardKeyEvent & { bind?: string | null; state?: string }>(
      "board-event",
      (event) => {
        const payload = event.payload;
        const raw = JSON.stringify(payload);
        if (logsPausedRef.current) {
          rawBufRef.current = [raw, ...rawBufRef.current].slice(0, 200);
        } else {
          setLogsRaw((items) => [raw, ...items].slice(0, 200));
        }
        if (payload.kind === "key" || payload.kind === "encoder") {
          // The live key view follows the raw edges as they arrive.
          const now = Date.now();
          setLive((cur) => onBoardEvent(cur, payload, now));
        } else if (payload.kind === "voice") {
          setVoiceActive(payload.state === "start");
        }
      },
    );

    const unlistenStatus = listen<KcStatus>("kc-status", (event) => {
      const s = event.payload;
      setKcStatuses((prev) => ({ ...prev, [s.session_id]: s }));
    });

    const unlistenCrewConnection = listen<CrewConnection>("crew-api-connection", (event) => {
      const c = event.payload;
      setStatus((current) => ({
        ...current,
        crew_api_connected: c.connected,
        crew_api_base_url: c.base_url,
        crew_api_last_error: c.error,
      }));
    });

    const unlistenCrewSlots = listen<CrewSlot[]>("crew-api-slots", (event) => {
      const slots = event.payload;
      setCrewSlots(slots);
      setStatus((current) => ({ ...current, crew_api_slots: slots.length }));
      setCrewSelectedSlot((current) =>
        current && slots.some((slot) => slot.key === current) ? current : (slots[0]?.key ?? ""),
      );
    });

    // A slot was bound or unbound on the keyboard (Settings > Bind session).
    const unlistenBindsChanged = listen<number>("kc-binds-changed", () => {
      void refreshBinds().catch(() => undefined);
    });

    const unlistenCrewActiveSlot = listen<string>("crew-api-active-slot", (event) => setCrewSelectedSlot(event.payload));

    const unlistenCrewDraft = listen<{ slot: string; text: string }>("crew-api-draft", (event) => {
      setCrewSelectedSlot(event.payload.slot);
      setCrewMessage(event.payload.text);
    });

    const unlistenVoiceSource = listen<string>("voice-source", (event) => {
      setVoiceSource(event.payload === "cloud" ? "cloud" : "system");
    });

    const unlistenWifi = listen<{ state: string; ssid: string }>("wifi-status", (event) => {
      const { state, ssid } = event.payload;
      setWifiStatus(ssid ? `${state} · ${ssid}` : state);
      setActivationStatus((current) => {
        const pre = ["unknown", "pairing", "connecting_router", "wifi_connected", "router_failed"].includes(current.state);
        if (!pre) return current;
        if (state === "connected") return { state: "wifi_connected", qr_url: "" };
        if (state === "failed" || state === "disconnected") return { state: "router_failed", qr_url: "" };
        if (state === "connecting") return { state: "connecting_router", qr_url: "" };
        return current;
      });
    });

    const unlistenActivation = listen<ActivationStatus>("activation-status", (event) => {
      const next = event.payload;
      setActivationStatus(next);
      if (next.state === "scan_qr" || next.state === "token_received" || next.state === "activated") {
        setActivationExpanded(true);
      } else if (next.state === "cloud_ready" || next.state === "resetting") {
        setActivationExpanded(false);
      }
    });

    const unlistenLicense = listen<string>("license-status", (event) => setLicStatus(event.payload));

    // Product ID write result (the PID itself never comes back).
    const unlistenPid = listen<PidStatus>("pid-status", (event) => {
      const { state, reason } = event.payload;
      setPidStatus(state === "rejected" ? `rejected · ${reason}` : state);
    });

    const unlistenLicenseInfo = listen<LicenseInfo>("license-info", (event) => {
      setLicInfo(event.payload);
      setLicStatus(`read · ${event.payload.source}`);
    });

    const unlistenDeviceSettings = listen<DeviceSettings>("device-settings", (event) => {
      setDeviceSettings(event.payload);
      setStatus((current) => ({ ...current, device_settings_synced: true }));
      if (isLocale(event.payload.language)) setLocale(event.payload.language);
    });

    invoke<string>("status_token")
      .then((token) => {
        if (token) {
          setSsToken(token);
          setSsRunning(true);
        }
      })
      .catch(() => undefined);
    invoke<ChatNumbers>("kc_bind_get").then(setBinds).catch(() => undefined);
    invoke<ChatAliases>("kc_bind_aliases_get").then(setBindAliases).catch(() => undefined);
    invoke<ChatSessions>("kc_bind_sessions_get").then(setBindSessions).catch(() => undefined);
    invoke<string>("kc_get_pinned")
      .then((sid) => setPinnedSession(sid || ""))
      .catch(() => undefined);
    invoke<CrewSlot[]>("crew_api_list_slots")
      .then((slots) => {
        setCrewSlots(slots);
        setCrewSelectedSlot((current) => current || slots[0]?.key || "");
      })
      .catch(() => undefined);
    invoke<HookStatus>("kc_hook_status").then(setHookStatus).catch(() => undefined);

    return () => {
      window.clearInterval(timer);
      unlistenLog.then((fn) => fn());
      unlistenBoard.then((fn) => fn());
      unlistenStatus.then((fn) => fn());
      unlistenCrewConnection.then((fn) => fn());
      unlistenCrewSlots.then((fn) => fn());
      unlistenCrewActiveSlot.then((fn) => fn());
      unlistenBindsChanged.then((fn) => fn());
      unlistenCrewDraft.then((fn) => fn());
      unlistenVoiceSource.then((fn) => fn());
      unlistenWifi.then((fn) => fn());
      unlistenActivation.then((fn) => fn());
      unlistenLicense.then((fn) => fn());
      unlistenPid.then((fn) => fn());
      unlistenLicenseInfo.then((fn) => fn());
      unlistenDeviceSettings.then((fn) => fn());
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return {
    t,
    locale,
    chooseLocale,
    status,
    board,
    uiError,
    setUiError,
    usbOk,
    provWindowOpen,
    live,
    voiceActive,
    voiceLabel:
      voiceActive
        ? t("simple.voice.listening")
        : t("simple.voice.idle"),
    deviceSettings,
    deviceSettingsBusy,
    adjustDeviceSetting,
    // Connection
    ports,
    boardPorts,
    connPrefs,
    updateConnPrefs,
    bleDevices,
    bleScanning,
    connect,
    disconnect,
    scanBle,
    refreshRuntime,
    runAction,
    voiceSource,
    selectVoiceSource,
    wifiSsid,
    setWifiSsid,
    wifiPass,
    setWifiPass,
    wifiStatus,
    wifiBusy,
    sendWifi,
    activationStatus,
    activationExpanded,
    setActivationExpanded,
    activationQrDrawing,
    activationQrError,
    activationStepIndex,
    licUuid,
    setLicUuid,
    licAuthkey,
    setLicAuthkey,
    licStatus,
    licBusy,
    licInfo,
    sendLicense,
    readLicense,
    pidInput,
    setPidInput,
    pidStatus,
    pidBusy,
    sendPid,
    pairingBusy,
    pairingStatus,
    enterPairing,
    // Input
    toggleControl,
    toggleFocusOnSelect,
    toggleShowWorkers,
    setVoiceForceKeyboard,
    logs,
    logsRaw,
    logsPaused,
    pausedCount,
    toggleLogsPause,
    clearLogs,
    // Control: Crew API
    crewSlots,
    crewSelectedSlot,
    setCrewSelectedSlot,
    crewMessage,
    setCrewMessage,
    crewApiBusy,
    crewApiError,
    runCrewApi,
    refreshCrewSlots,
    sendCrewApiMessage,
    crewSlotAction,
    startCrewApi: () => invoke("crew_api_start"),
    stopCrewApi: () => invoke("crew_api_stop"),
    isIdentityFailure,
    // Control: A/B/C keys and watched chats
    binds,
    bindAliases,
    bindSessions,
    bindError,
    bindBusy,
    runBind,
    bindSet,
    bindSession,
    clearBoundSession,
    bindAliasDraft,
    bindAliasSave,
    aliasMaxChars: ALIAS_MAX_CHARS,
    sortedKcStatuses,
    watchedView,
    setWatchedView,
    quickBindDraft,
    setQuickBindDraft,
    startQuickBind,
    confirmQuickBind,
    pinnedSession,
    pinSession,
    unpinSession,
    // Control: legacy and hook
    lcTitle,
    setLcTitle,
    lcBound,
    lcText,
    setLcText,
    lcResult,
    lcError,
    lcBusy,
    runLc,
    lcBind,
    lcSendText,
    lcCommandBar,
    lcChord,
    ssRunning,
    ssToken,
    ssError,
    ssBusy,
    runSs,
    ssStart,
    ssStop,
    hookStatus,
    hookRefresh,
    hookOpenFolder,
    hookInstall,
    hookRemove,
    applyKeymap,
  };
}
