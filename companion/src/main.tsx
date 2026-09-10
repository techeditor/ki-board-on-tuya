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
// reference mapping placeholder — remapping UI lands in a later pass.
const KEY_MAP: { id: string; label: string; intent: string }[] = [
  { id: "k1", label: "K1", intent: "Voice (push-to-talk)" },
  { id: "k2", label: "K2", intent: "Send / Enter" },
  { id: "k3", label: "K3", intent: "Cancel / Esc" },
  { id: "k4", label: "K4", intent: "Backspace" },
  { id: "k5", label: "K5", intent: "Unassigned" },
];

// 3×3 grid positions for the 5-way navigation switch.
const NAV_LAYOUT: (string | null)[] = [
  null, "up", null,
  "left", "center", "right",
  null, "down", null,
];

function App() {
  const [status, setStatus] = useState<Status>(defaultStatus);
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [selectedPort, setSelectedPort] = useState("");
  const [logs, setLogs] = useState<string[]>([]);
  const [voiceEngine, setVoiceEngine] = useState<"system" | "third_party">("system");
  const [lastKey, setLastKey] = useState("");
  const [lastNav, setLastNav] = useState("");
  const [voiceActive, setVoiceActive] = useState(false);
  const [activeView, setActiveView] = useState<"device" | "input" | "logs">("device");
  const [uiError, setUiError] = useState("");

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
    await invoke("connect_device", { portName: selectedPort || null });
    await refreshRuntime();
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
        {(["device", "input", "logs"] as const).map((view) => (
          <button
            key={view}
            className={activeView === view ? "nav active" : "nav"}
            onClick={() => setActiveView(view)}
          >
            {view.toUpperCase()}
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
            <div className="actions">
              <button onClick={() => runAction(connect)}>Connect</button>
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
            <div className="key-grid" style={{ marginTop: 14 }}>
              {KEY_MAP.map((key) => (
                <div className={lastKey === key.id ? "key-card hit" : "key-card"} key={key.id}>
                  <div className="key-screen">{key.label}</div>
                  <div className="key-map">{key.intent}</div>
                </div>
              ))}
            </div>

            <label style={{ marginTop: 20 }}>5-way navigation</label>
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
    case "logs":
      return "Event Stream";
    default:
      return "Device Control";
  }
}

createRoot(document.getElementById("root")!).render(<App />);
