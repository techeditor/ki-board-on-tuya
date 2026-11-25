/* Logs (design §1): event stream in Detailed or Raw JSON form. Pause freezes
 * the visible list and buffers new frames; resume merges them back. */
import { useState } from "react";

import { Card } from "../../ui/primitives";
import type { Model } from "../useCompanion";

export function LogsView({ m }: { m: Model }) {
  const t = m.t;
  const [raw, setRaw] = useState(false);
  const lines = raw ? m.logsRaw : m.logs;

  return (
    <Card title={t("logs.title")}>
      <div className="kc-keymap-head" style={{ marginBottom: 10 }}>
        <button type="button" className={`kc-seg ${!raw ? "on" : ""}`} onClick={() => setRaw(false)}>{t("logs.detailed")}</button>
        <button type="button" className={`kc-seg ${raw ? "on" : ""}`} onClick={() => setRaw(true)}>{t("logs.raw")}</button>
      </div>
      <div className="kc-keymap-actions" style={{ marginTop: 0, justifyContent: "flex-start" }}>
        <button type="button" className={m.logsPaused ? "kc-btn" : "kc-btn ghost"} onClick={m.toggleLogsPause}>
          {m.logsPaused ? `${t("logs.resume")}${m.pausedCount ? ` (+${m.pausedCount})` : ""}` : t("logs.pause")}
        </button>
        <button type="button" className="kc-btn ghost" onClick={() => m.runAction(m.clearLogs)}>{t("logs.clear")}</button>
      </div>
      <div className="kc-log">
        {lines.length === 0 ? (
          <span>{raw ? t("logs.emptyRaw") : t("logs.empty")}</span>
        ) : (
          lines.map((line, index) => <div key={`${index}-${line}`}>{line}</div>)
        )}
      </div>
    </Card>
  );
}
