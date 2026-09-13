import { useEffect, useMemo, useState, type ReactNode } from "react";
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

const defaultStatus: Status = {
  device_connected: false,
  transport: "none",
  endpoint: "",
  firmware_version: "",
  last_error: "",
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
  const [voiceEngine, setVoiceEngine] = useState<"system" | "third_party">("system");
  const [lastKey, setLastKey] = useState("");
  const [lastNav, setLastNav] = useState("");
  const [voiceActive, setVoiceActive] = useState(false);
  const [activeView, setActiveView] = useState<"device" | "input" | "kirocrew" | "logs">("device");
  const [uiError, setUiError] = useState("");

  // FP-4 Kiro Crew panel state.
  const [kcAgent, setKcAgent] = useState("kirocrew");
  const [kcTarget, setKcTarget] = useState("");
  const [kcMessage, setKcMessage] = useState("");
  const [kcReply, setKcReply] = useState("");
  const [kcError, setKcError] = useState("");
  const [kcBusy, setKcBusy] = useState(false);

  const boardPorts = useMemo(
    () => ports.filter((port) => port.kind !== "flash"),
    [ports],
  );

  async function refreshRuntime() {
    setStatus(await invoke<Status>("get_status"));
    setPorts(await invoke<SerialPortInfo[]>("list_serial_ports"));
    setLogs(await invoke<string[]>("get_logs"));
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
  }

  async function selectVoiceEngine(engine: "system" | "third_party") {
    setVoiceEngine(engine);
    // Only pushes to the board when connected; ignore the "not connected" error.
    await invoke("send_config", { voiceEngine: engine }).catch(() => undefined);
  }

  // ── FP-4 Kiro Crew session control (route A) ──────────────────────────────
  // Wraps a kc_* invoke with local busy/error handling. Arg names match the
  // #[tauri::command] signatures in src-tauri/src/lib.rs exactly:
  //   kc_create(title, agent) -> String       kc_send(message, target?)
  //   kc_read(target?) -> String              kc_stop(target?)
  async function runKc(action: () => Promise<void>) {
    setKcBusy(true);
    try {
      setKcError("");
      await action();
    } catch (error) {
      setKcError(error instanceof Error ? error.message : String(error));
    } finally {
      setKcBusy(false);
    }
  }

  async function kcBind() {
    const agent = kcAgent.trim();
    if (!agent) {
      setKcError("Enter an agent name before binding a session.");
      return;
    }
    const id = await invoke<string>("kc_create", {
      title: `Board control · ${agent}`,
      agent,
    });
    setKcTarget(id);
    setKcReply("");
  }

  async function kcSend() {
    const message = kcMessage.trim();
    if (!message) {
      setKcError("Type a message before sending.");
      return;
    }
    // Pass the bound target explicitly; the backend also falls back to it.
    await invoke("kc_send", { message, target: kcTarget || null });
    setKcMessage("");
  }

  async function kcRead() {
    const reply = await invoke<string>("kc_read", { target: kcTarget || null });
    setKcReply(reply);
  }

  async function kcStop() {
    await invoke("kc_stop", { target: kcTarget || null });
    setKcTarget("");
    setKcReply("");
  }

  useEffect(() => {
    refreshRuntime().catch(() => undefined);
    const timer = window.setInterval(() => {
      invoke<Status>("get_status").then(setStatus).catch(() => undefined);
    }, 1500);

    const unlistenLog = listen<string>("companion-log", (event) => {
      setLogs((items) => [event.payload, ...items].slice(0, 200));
    });

    const unlistenBoard = listen<BoardEvent>("board-event", (event) => {
      const payload = event.payload;
      if (payload.kind === "key" && payload.id) {
        if (payload.action === "down" || payload.action === "long") {
          setLastKey(payload.id);
        }
      } else if (payload.kind === "nav" && payload.dir) {
        if (payload.action === "down" || payload.action === "long") {
          setLastNav(payload.dir);
        }
      } else if (payload.kind === "voice") {
        setVoiceActive(payload.state === "start");
      }
    });

    return () => {
      window.clearInterval(timer);
      unlistenLog.then((fn) => fn());
      unlistenBoard.then((fn) => fn());
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
            {view === "kirocrew" ? "KIRO CREW" : view.toUpperCase()}
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
                  BLE GATT · service <code>0x1910</code>, notify <code>0x2B10</code>, write
                  <code> 0x2B11</code>. Scan (~4s) finds boards advertising <code>AK-KB</code>; the
                  companion greets only after notify subscription is live.
                </p>
              </>
            )}

            <div className="actions">
              <button className="secondary" onClick={() => runAction(connect)}>Connect</button>
              <button className="secondary" onClick={() => runAction(disconnect)}>Disconnect</button>
              <button className="secondary" onClick={() => runAction(refreshRuntime)}>Refresh</button>
            </div>

            <label>Voice engine</label>
            <div className="actions" style={{ marginTop: 6 }}>
              <button
                className={voiceEngine === "system" ? "" : "secondary"}
                onClick={() => runAction(() => selectVoiceEngine("system"))}
              >
                System (Windows ASR)
              </button>
              <button
                className={voiceEngine === "third_party" ? "" : "secondary"}
                onClick={() => runAction(() => selectVoiceEngine("third_party"))}
              >
                Third-party ASR
              </button>
            </div>
            <p className="hint">
              Phase 1 uses the Windows system recognizer. The choice is pushed to the board as a
              <code> config </code> frame when connected.
            </p>
          </Panel>
        )}

        {activeView === "input" && (
          <Panel title="Live Input">
            <p className="hint">
              Highlights the last key / direction reported by the board. Default intents shown are a
              placeholder for the remapping UI.
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
          <Panel title="Kiro Crew Control">
            <p className="hint">
              Route A · session-control API on <code>127.0.0.1:5476</code>. Bind an agent session,
              then send messages as agent turns and read replies.
            </p>

            <label>Agent name</label>
            <input
              value={kcAgent}
              onChange={(event) => setKcAgent(event.target.value)}
              placeholder="kirocrew"
            />
            <div className="actions" style={{ marginTop: 6 }}>
              <button disabled={kcBusy} onClick={() => runKc(kcBind)}>
                Bind session
              </button>
              <button
                className="secondary"
                disabled={kcBusy || !kcTarget}
                onClick={() => runKc(kcStop)}
              >
                Stop
              </button>
            </div>

            <div className="lcd" style={{ marginTop: 14 }}>
              <span>BOUND SESSION</span>
              <strong>{kcTarget || "-- (bind first)"}</strong>
            </div>

            <label style={{ marginTop: 18 }}>Message</label>
            <input
              value={kcMessage}
              onChange={(event) => setKcMessage(event.target.value)}
              placeholder="Ask the agent to do something…"
              onKeyDown={(event) => {
                if (event.key === "Enter" && !kcBusy) runKc(kcSend);
              }}
            />
            <div className="actions" style={{ marginTop: 6 }}>
              <button disabled={kcBusy || !kcTarget} onClick={() => runKc(kcSend)}>
                Send
              </button>
              <button
                className="secondary"
                disabled={kcBusy || !kcTarget}
                onClick={() => runKc(kcRead)}
              >
                Read reply
              </button>
            </div>

            <label style={{ marginTop: 18 }}>Last reply</label>
            <div className="terminal" style={{ minHeight: 120, whiteSpace: "pre-wrap" }}>
              {kcReply || "No reply yet. Send a message, then read the reply."}
            </div>

            {kcError && <div className="error-banner" style={{ marginTop: 14 }}>{kcError}</div>}
          </Panel>
        )}

        {activeView === "logs" && (
          <Panel title="Event Stream">
            <div className="actions" style={{ marginTop: 0 }}>
              <button className="secondary" onClick={() => runAction(clearLogs)}>Clear</button>
            </div>
            <div className="log">
              {logs.length === 0
                ? "No events yet. Connect the board to see key / nav / voice frames."
                : logs.map((line, index) => <div key={`${index}-${line}`}>{line}</div>)}
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
      return "Kiro Crew Control";
    case "logs":
      return "Event Stream";
    default:
      return "Device Control";
  }
}

createRoot(document.getElementById("root")!).render(<App />);
