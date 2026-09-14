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
  kind: "key" | "nav" | "voice" | "macro" | "hello_ack" | "ping" | "pong" | "unknown";
  seq?: number | null;
  id?: string;
  dir?: string;
  action?: string;
  held_ms?: number;
  state?: string;
  name?: string;
  type?: string;
};

// FP-4b STATUS: a mapped agent status pushed from the hook sink via `kc-status`.
type KcStatus = {
  session_id: string;
  agent_name: string;
  state: string;
  text: string;
  // Set by the sink: true when this session is the one currently driving the
  // board LCD (either nothing is pinned, or this is the pinned session).
  forwarded?: boolean;
};

// FP-4b: one-click global hook install status, mirroring the Rust
// hook_install::HookStatus / AgentHookState.
type AgentHookState = { agent: string; installed_events: number };
type HookStatus = {
  command: string;
  agents_dir: string;
  agents: AgentHookState[];
  fully_installed: boolean;
  any_installed: boolean;
};

// FP-4b M4 (§3.2): the three per-key chat NUMBERS (K1-K3). Each key jumps to a// chat number (1..9) via Kiro Crew's "Alt+N = Jump to chat N" shortcut —
// client-side, no page reload. The number is user-configurable per key. This
// mirrors the Rust `bind_store::BindStore.chats: Vec<u8>` (index 0 = K1).
type ChatNumbers = number[];

const defaultStatus: Status = {
  device_connected: false,
  transport: "none",
  endpoint: "",
  firmware_version: "",
  last_error: "",
  control_enabled: false,
};

// Our 5 mechanical keys (k1..k5) with their phase-1 default intents. This is a
// reference mapping placeholder — remapping UI lands in a later pass. `wide`
// marks K4 as the double-width push-to-talk key in the INPUT layout.
const KEY_MAP: { id: string; label: string; intent: string; wide?: boolean }[] = [
  { id: "k1", label: "K1", intent: "Agent slot 1" },
  { id: "k2", label: "K2", intent: "Agent slot 2" },
  { id: "k3", label: "K3", intent: "Agent slot 3" },
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
  const [voiceSource, setVoiceSource] = useState<"cloud" | "system">("system");
  // WiFi provisioning (DEVICE panel): send SSID/pass to the board over the link.
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [wifiStatus, setWifiStatus] = useState("");
  const [wifiBusy, setWifiBusy] = useState(false);
  // Device license (UUID/AuthKey) provisioning.
  const [licUuid, setLicUuid] = useState("");
  const [licAuthkey, setLicAuthkey] = useState("");
  const [licStatus, setLicStatus] = useState("");
  const [licBusy, setLicBusy] = useState(false);
  const [lastKey, setLastKey] = useState("");
  const [lastNav, setLastNav] = useState("");
  const [voiceActive, setVoiceActive] = useState(false);
  const [activeView, setActiveView] = useState<"device" | "input" | "kirocrew" | "logs">("device");
  const [uiError, setUiError] = useState("");

  // FP-4b route B ("Local Control", the hands) state: focus a Kiro Crew window
  // and inject keystrokes.
  const [lcTitle, setLcTitle] = useState("");
  const [lcBound, setLcBound] = useState(false);
  const [lcText, setLcText] = useState("");
  const [lcResult, setLcResult] = useState("");
  const [lcError, setLcError] = useState("");
  const [lcBusy, setLcBusy] = useState(false);

  // FP-4b M4 (§3.2): the three per-key chat NUMBERS (K1-K3). Each key fires
  // Alt+<number> to jump to that chat (client-side, no reload). Loaded via
  // kc_bind_get on mount; changing a key's number calls kc_bind_set(slot, n).
  const [binds, setBinds] = useState<ChatNumbers>([]);
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
  // FP-4b Route STATUS (option A): the pinned "watched chat". "" = follow all
  // sessions. Non-empty = only that session's status drives the board LCD.
  const [pinnedSession, setPinnedSession] = useState("");
  // FP-4b: selectable hook installation state. On first load every discovered
  // agent is selected (preserves the former install-all default); users may
  // then select individuals, select all, or clear the selection.
  const [hookStatus, setHookStatus] = useState<HookStatus | null>(null);
  const [selectedHookAgents, setSelectedHookAgents] = useState<string[]>([]);
  const hookSelectionReadyRef = useRef(false);

  const boardPorts = useMemo(
    () => ports.filter((port) => port.kind !== "flash"),
    [ports],
  );

  async function refreshRuntime() {
    setStatus(await invoke<Status>("get_status"));
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
    await refreshRuntime();
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

  // Enable/disable routing board keys to Kiro Crew control. Default OFF so the
  // INPUT view only reflects trigger state; the board never drives Kiro Crew
  // until the user explicitly turns this on.
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

  // ── FP-4b M4: per-key chat numbers (§3.2) ─────────────────────────────────
  // Arg names match the #[tauri::command] signatures in src-tauri/src/lib.rs:
  //   kc_bind_get() -> number[]      kc_bind_set(slot, number)
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
    setBinds(await invoke<ChatNumbers>("kc_bind_get"));
  }

  // Set slot `slot` (1-based) to chat `number` (1..9).
  async function bindSet(slot: number, number: number) {
    await invoke("kc_bind_set", { slot, number });
    await refreshBinds();
  }

  // Reset slot `slot` to its default number (slot index+1: K1->1, K2->2, K3->3).
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

  // FP-4b: selected-agent hook install/remove/status. On first discovery,
  // select all agents to preserve the former one-click install-all behavior.
  // Later refreshes preserve the user's selection and drop names that vanished.
  function acceptHookStatus(st: HookStatus) {
    setHookStatus(st);
    const available = st.agents.map((a) => a.agent);
    setSelectedHookAgents((previous) => {
      if (!hookSelectionReadyRef.current) {
        hookSelectionReadyRef.current = true;
        return available;
      }
      const availableSet = new Set(available);
      return previous.filter((agent) => availableSet.has(agent));
    });
  }

  async function hookRefresh() {
    acceptHookStatus(await invoke<HookStatus>("kc_hook_status", { exe: null }));
  }

  async function hookOpenAgentsFolder() {
    await invoke<string>("kc_open_agents_folder");
  }

  async function hookInstall() {
    acceptHookStatus(
      await invoke<HookStatus>("kc_hook_install", {
        exe: null,
        agents: selectedHookAgents,
      }),
    );
  }

  async function hookRemove() {
    acceptHookStatus(
      await invoke<HookStatus>("kc_hook_remove", {
        exe: null,
        agents: selectedHookAgents,
      }),
    );
  }

  function toggleHookAgent(agent: string) {
    setSelectedHookAgents((selected) =>
      selected.includes(agent)
        ? selected.filter((name) => name !== agent)
        : [...selected, agent],
    );
  }

  useEffect(() => {
    refreshRuntime().catch(() => undefined);
    const timer = window.setInterval(() => {
      invoke<Status>("get_status").then(setStatus).catch(() => undefined);
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

    // Recover the sink token/running state on mount (survives a UI reload).
    invoke<string>("status_token")
      .then((token) => {
        if (token) {
          setSsToken(token);
          setSsRunning(true);
        }
      })
      .catch(() => undefined);

    // Load the persisted M4 per-key chat numbers on mount.
    invoke<ChatNumbers>("kc_bind_get")
      .then(setBinds)
      .catch(() => undefined);

    // Recover the pinned "watched chat" on mount (survives a UI reload).
    invoke<string>("kc_get_pinned")
      .then((sid) => setPinnedSession(sid || ""))
      .catch(() => undefined);

    // Load hook status and select all discovered agents on first mount.
    invoke<HookStatus>("kc_hook_status", { exe: null })
      .then(acceptHookStatus)
      .catch(() => undefined);

    return () => {
      window.clearInterval(timer);
      unlistenLog.then((fn) => fn());
      unlistenBoard.then((fn) => fn());
      unlistenStatus.then((fn) => fn());
      unlistenVoiceSource.then((fn) => fn());
      unlistenWifi.then((fn) => fn());
      unlistenLicense.then((fn) => fn());
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
              onChange={(event) => setWifiSsid(event.target.value)}
              placeholder="WiFi SSID (2.4GHz)"
            />
            <input
              type="password"
              value={wifiPass}
              onChange={(event) => setWifiPass(event.target.value)}
              placeholder="WiFi password"
              style={{ marginTop: 6 }}
              onKeyDown={(event) => {
                if (event.key === "Enter" && !wifiBusy) runAction(sendWifi);
              }}
            />
            <div className="actions" style={{ marginTop: 6 }}>
              <button disabled={wifiBusy} onClick={() => runAction(sendWifi)}>
                Configure WiFi
              </button>
            </div>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>WIFI</span>
              <strong className={wifiStatus.startsWith("connected") ? "good-text" : ""}>
                {wifiStatus || "-- (not configured)"}
              </strong>
            </div>
            <p className="hint">
              Connect the board first (USB/BLE), then send WiFi credentials — the board joins the
              AP over its own radio (needs the cloud-ASR firmware profile; 2.4GHz only).
            </p>

            <label style={{ marginTop: 14 }}>Device license (UUID / AuthKey)</label>
            <input
              value={licUuid}
              onChange={(event) => setLicUuid(event.target.value)}
              placeholder="UUID (20 chars)"
            />
            <input
              type="password"
              value={licAuthkey}
              onChange={(event) => setLicAuthkey(event.target.value)}
              placeholder="AuthKey (32 chars)"
              style={{ marginTop: 6 }}
            />
            <div className="actions" style={{ marginTop: 6 }}>
              <button disabled={licBusy} onClick={() => runAction(sendLicense)}>
                Write license
              </button>
            </div>
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>LICENSE</span>
              <strong className={licStatus.startsWith("written") ? "good-text" : ""}>
                {licStatus || "-- (not written)"}
              </strong>
            </div>
            <p className="hint">
              Writes the per-device UUID/AuthKey to the board's persistent storage (needs the
              cloud-ASR profile). The PID is compile-time (product-wide). The secret only travels
              to the board — it is never stored or echoed by the companion.
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
              Default is <strong>disabled</strong>: pressing keys only checks the trigger state here
              and never drives Kiro Crew. Enable to let board keys/nav control the local Kiro Crew
              window.
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
            <Panel title="Bindings — K1–K3 jump-to-chat number">
              <p className="hint">
                Bind each slot key (K1–K3) to a Kiro Crew <strong>chat number</strong> (1–9).
                Pressing the board key fires <code>Alt+&lt;number&gt;</code> — Kiro Crew's
                <em> "Jump to chat N"</em> shortcut — switching to that chat <strong>client-side,
                with no page reload</strong>. The number is configurable per key. Requires
                <em> Local Control</em> below to be bound to a Kiro Crew window and control enabled.
              </p>

              {[1, 2, 3].map((slot) => {
                const n = binds[slot - 1];
                return (
                  <div key={slot} style={{ marginTop: slot === 1 ? 6 : 12 }}>
                    <div style={{ fontSize: 13 }}>
                      <strong>{`K${slot}: `}</strong>
                      <span className="good-text">
                        {n ? `Alt+${n} (jump to chat ${n})` : "--"}
                      </span>
                    </div>
                    <div className="actions" style={{ marginTop: 6, flexWrap: "nowrap", alignItems: "center", gap: 8 }}>
                      <label style={{ margin: 0, whiteSpace: "nowrap" }}>Chat #</label>
                      <select
                        value={n ?? ""}
                        disabled={bindBusy}
                        style={{ width: "auto", minWidth: 56 }}
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
                        className="secondary"
                        disabled={bindBusy}
                        style={{ whiteSpace: "nowrap" }}
                        onClick={() => runBind(() => bindClear(slot))}
                      >
                        Reset to {slot}
                      </button>
                    </div>
                  </div>
                );
              })}

              {bindError && <div className="error-banner" style={{ marginTop: 14 }}>{bindError}</div>}
            </Panel>

            <Panel title="Local Control — the hands (route B)">
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

            <Panel title="Agent Status — the eyes (hook sink)">
              <p className="hint">
                Ingests Kiro Crew <strong>agent hooks</strong> on
                <code> 127.0.0.1:5477/status</code> and mirrors each agent's state to the board
                LCD as a <code>display</code> frame. The hook is a mode of this very binary
                (<code>companion --hook</code>), gated by a per-run token.
              </p>

              <label style={{ marginTop: 8 }}>Hook installation (choose local agents)</label>
              <p className="hint" style={{ marginTop: 4 }}>
                Choose which agents should run <code>companion --hook</code> for
                <em> agentSpawn / postToolUse / stop</em>. Install and Remove affect only the
                checked agents under <code>{hookStatus?.agents_dir ?? "~/.kiro/agents"}</code>;
                hand-authored hooks and unchecked agents are left untouched.
              </p>
              <div className="lcd" style={{ marginTop: 6 }}>
                <span>HOOK</span>
                <strong className={hookStatus?.fully_installed ? "good-text" : ""}>
                  {!hookStatus
                    ? "-- (checking)"
                    : hookStatus.agents.length === 0
                    ? "NO AGENTS FOUND"
                    : hookStatus.fully_installed
                    ? `INSTALLED · ${hookStatus.agents.length}/${hookStatus.agents.length}`
                    : hookStatus.any_installed
                    ? `PARTIAL · ${hookStatus.agents.filter((a) => a.installed_events === 3).length}/${hookStatus.agents.length}`
                    : "NOT INSTALLED"}
                </strong>
              </div>
              <div className="actions" style={{ marginTop: 6 }}>
                <button
                  className="secondary"
                  disabled={
                    ssBusy ||
                    !hookStatus ||
                    selectedHookAgents.length === hookStatus.agents.length
                  }
                  onClick={() =>
                    setSelectedHookAgents(hookStatus?.agents.map((a) => a.agent) ?? [])
                  }
                >
                  Select all
                </button>
                <button
                  className="secondary"
                  disabled={ssBusy || selectedHookAgents.length === 0}
                  onClick={() => setSelectedHookAgents([])}
                >
                  Clear selection
                </button>
                <button className="secondary" disabled={ssBusy} onClick={() => runSs(hookRefresh)}>
                  Refresh
                </button>
                <button
                  className="secondary"
                  disabled={ssBusy}
                  onClick={() => runSs(hookOpenAgentsFolder)}
                >
                  Open agent folder
                </button>
              </div>
              {hookStatus && hookStatus.agents.length > 0 && (
                <div className="log" style={{ marginTop: 6 }}>
                  {hookStatus.agents.map((a) => {
                    const selected = selectedHookAgents.includes(a.agent);
                    const installState =
                      a.installed_events === 3
                        ? "installed"
                        : a.installed_events > 0
                        ? `partial ${a.installed_events}/3`
                        : "not installed";
                    return (
                      <label
                        key={a.agent}
                        style={{
                          alignItems: "center",
                          cursor: ssBusy ? "default" : "pointer",
                          display: "flex",
                          gap: 8,
                          margin: 0,
                          opacity: selected ? 1 : 0.65,
                          textTransform: "none",
                        }}
                      >
                        <input
                          type="checkbox"
                          checked={selected}
                          disabled={ssBusy}
                          onChange={() => toggleHookAgent(a.agent)}
                          style={{ width: "auto" }}
                        />
                        <span>{a.agent}</span>
                        <span style={{ marginLeft: "auto" }}>{installState}</span>
                      </label>
                    );
                  })}
                </div>
              )}
              <div className="actions" style={{ marginTop: 6 }}>
                <button
                  disabled={ssBusy || selectedHookAgents.length === 0}
                  onClick={() => runSs(hookInstall)}
                >
                  Install selected ({selectedHookAgents.length})
                </button>
                <button
                  className="secondary"
                  disabled={
                    ssBusy ||
                    selectedHookAgents.length === 0 ||
                    !hookStatus?.agents.some(
                      (a) => selectedHookAgents.includes(a.agent) && a.installed_events > 0,
                    )
                  }
                  onClick={() => runSs(hookRemove)}
                >
                  Remove selected ({selectedHookAgents.length})
                </button>
              </div>
              <p className="hint" style={{ marginTop: 4, opacity: 0.7 }}>
                Note: a Kiro Crew upgrade/rebuild can rewrite agent specs and drop these entries —
                if status ever falls back to NOT INSTALLED after an update, click Install again.
              </p>

              <div className="actions" style={{ marginTop: 6 }}>
                <button disabled={ssBusy || ssRunning} onClick={() => runSs(ssStart)}>
                  Start sink
                </button>
                <button
                  className="secondary"
                  disabled={ssBusy || !ssRunning}
                  onClick={() => runSs(ssStop)}
                >
                  Stop sink
                </button>
              </div>

              <div className="lcd" style={{ marginTop: 14 }}>
                <span>SINK</span>
                <strong>{ssRunning ? "LISTENING · 127.0.0.1:5477" : "-- (start first)"}</strong>
              </div>

              {ssRunning && (
                <>
                  <label style={{ marginTop: 18 }}>Token (X-AK-Status-Token)</label>
                  <input readOnly value={ssToken} onFocus={(e) => e.currentTarget.select()} />

                  <label style={{ marginTop: 18 }}>Hook command (paste into your Kiro agent config)</label>
                  <div className="terminal" style={{ whiteSpace: "pre-wrap" }}>
                    {'"<path-to-companion.exe>" --hook'}
                  </div>
                  <p className="hint">
                    Add this command to your <strong>local</strong> agent config
                    (<code>~/.kiro/agents/&lt;name&gt;.json</code>) under <code>hooks</code> for
                    <code> agentSpawn</code>, <code>postToolUse</code> and <code>stop</code>. The
                    hook reads the sink's port + token from
                    <code> %LOCALAPPDATA%\\ak-companion\\hook.json</code> (Windows) /
                    <code> ~/.config/ak-companion/hook.json</code> (else), so no token needs to
                    be pasted into the config — starting the sink writes it. Example:
                  </p>
                  <div className="terminal" style={{ whiteSpace: "pre-wrap" }}>
{`{
  "hooks": {
    "agentSpawn":  [{ "command": "\\"C:/path/to/companion.exe\\" --hook" }],
    "postToolUse": [{ "command": "\\"C:/path/to/companion.exe\\" --hook" }],
    "stop":        [{ "command": "\\"C:/path/to/companion.exe\\" --hook" }]
  }
}`}
                  </div>
                </>
              )}

              <label style={{ marginTop: 18 }}>
                Watched chat (drives the board LCD)
              </label>
              <p className="hint" style={{ marginTop: 4 }}>
                {pinnedSession
                  ? `Pinned — only session ${pinnedSession.slice(0, 8)}… updates the board. Click another to switch, or Follow all.`
                  : "Following ALL sessions — any active chat updates the board. Click one to pin it as the only watched chat."}
              </p>
              <div className="actions" style={{ marginTop: 0 }}>
                <button
                  className={pinnedSession ? "secondary" : ""}
                  disabled={!pinnedSession}
                  onClick={() => runSs(unpinSession)}
                >
                  Follow all
                </button>
              </div>
              <div className="log">
                {Object.keys(kcStatuses).length === 0
                  ? "No status yet. Start the sink, then run an agent turn with the hook wired."
                  : Object.values(kcStatuses)
                      .sort((a, b) =>
                        a.session_id === pinnedSession
                          ? -1
                          : b.session_id === pinnedSession
                          ? 1
                          : a.session_id.localeCompare(b.session_id),
                      )
                      .map((s) => {
                        const isPinned = s.session_id === pinnedSession;
                        const driving = pinnedSession ? isPinned : true;
                        return (
                          <div
                            key={s.session_id}
                            onClick={() => runSs(() => pinSession(s.session_id))}
                            title={`Pin ${s.session_id} as the watched chat`}
                            style={{
                              cursor: "pointer",
                              fontWeight: isPinned ? 700 : 400,
                              opacity: driving ? 1 : 0.5,
                            }}
                          >
                            {`${isPinned ? "📌 " : "   "}${s.session_id.slice(0, 8)} · ${s.state.toUpperCase()} · ${s.text}`}
                          </div>
                        );
                      })}
              </div>

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
