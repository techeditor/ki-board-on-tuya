/* Control (design §4): API + WebSocket is the primary path and is expanded by
 * default; the legacy shortcut chain sits in a disclosure. The mechanism choice
 * is persisted by the shell. Slot keys identify chats on the API path, so the
 * A/B/C keys bind to stable chats without a window-focus binding step. */
import type { ReactNode } from "react";

import { Card, StateBadge } from "../../ui/primitives";
import { SLOT_DEFAULT_NAMES, isIdentityFailure, sessionDisplayName, type KcStatus } from "../companionTypes";
import type { Model } from "../useCompanion";

export type Mechanism = "api" | "legacy";

export function ControlView({
  m,
  mechanism,
  setMechanism,
}: {
  m: Model;
  mechanism: Mechanism;
  setMechanism: (value: Mechanism) => void;
}) {
  const t = m.t;
  const status = m.status;
  const connected = Boolean(status.crew_api_connected);
  const selected = m.crewSlots.find((s) => s.key === m.crewSelectedSlot);

  return (
    <div className="kc-stack">
      <div className="kc-keymap-head" style={{ marginBottom: 0, alignSelf: "flex-start" }}>
        <button type="button" className={`kc-seg ${mechanism === "api" ? "on" : ""}`} onClick={() => setMechanism("api")}>
          {t("control.mech.api")}
        </button>
        <button type="button" className={`kc-seg ${mechanism === "legacy" ? "on" : ""}`} onClick={() => setMechanism("legacy")}>
          {t("control.mech.legacy")}
        </button>
      </div>

      {mechanism === "api" && (
        <>
          <Card title={t("control.api.title")} info="help.control.api">
            <div className="kc-field" style={{ marginTop: 0 }}>
              <span>{t("control.api.endpoint")}</span>
              <code>{status.crew_api_base_url || "http://127.0.0.1:5476"}</code>
            </div>
            <div className="kc-field" style={{ marginTop: 10 }}>
              <Metric label={t("control.api.link")} value={connected ? t("control.api.connected") : t("control.api.reconnecting")} ok={connected} />
              <Metric label={t("control.api.chats")} value={String(m.crewSlots.length || status.crew_api_slots || 0)} />
            </div>
            <div className="kc-field">
              <button type="button" className="kc-btn ghost" disabled={m.crewApiBusy} onClick={() => m.runCrewApi(m.refreshCrewSlots)}>{t("common.refresh")}</button>
                  <button type="button" className="kc-btn ghost" disabled={m.crewApiBusy || connected} onClick={() => m.runCrewApi(m.startCrewApi)}>{t("control.api.wsStart")}</button>
              <button type="button" className="kc-btn ghost" disabled={m.crewApiBusy || !connected} onClick={() => m.runCrewApi(m.stopCrewApi)}>{t("control.api.wsStop")}</button>
            </div>

            <div className="kc-field" style={{ marginTop: 12, alignItems: "flex-start", flexDirection: "column" }}>
              <span>{t("control.api.chatList")}</span>
              {m.crewSlots.length === 0 ? (
                <span className="kc-muted">{t("control.api.none")}</span>
              ) : (
                m.crewSlots.map((slot) => {
                  const isWatched = m.pinnedSession === slot.key;
                  return (
                    <div className="kc-field" key={slot.key} style={{ marginTop: 4, width: "100%" }}>
                      <StateBadge tone={slot.running ? "accent" : "neutral"}>{slot.running ? "RUNNING" : "IDLE"}</StateBadge>
                      <button
                        type="button"
                        className="kc-seg"
                        aria-pressed={m.crewSelectedSlot === slot.key}
                        onClick={() => m.setCrewSelectedSlot(slot.key)}
                        style={{ textAlign: "left" }}
                      >
                        {slot.title || slot.key}
                      </button>
                      <code className="kc-muted">{slot.key}</code>
                      <button
                        type="button"
                        className={isWatched ? "kc-btn" : "kc-btn ghost"}
                        disabled={m.ssBusy}
                        onClick={() => m.runSs(() => (isWatched ? m.unpinSession() : m.pinSession(slot.key)))}
                      >
                        {isWatched ? t("control.watch.stop") : t("control.watch.start")}
                      </button>
                    </div>
                  );
                })
              )}
            </div>

            <div className="kc-field" style={{ marginTop: 12 }}>
              <input
                placeholder={t("control.api.messagePh")}
                value={m.crewMessage}
                disabled={m.crewApiBusy || !m.crewSelectedSlot}
                onChange={(e) => m.setCrewMessage(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" && !m.crewApiBusy) void m.runCrewApi(m.sendCrewApiMessage);
                }}
                style={{ flex: 1 }}
              />
              <button type="button" className="kc-btn" disabled={m.crewApiBusy || !m.crewSelectedSlot || !m.crewMessage.trim()} onClick={() => m.runCrewApi(m.sendCrewApiMessage)}>
                {t("control.api.send")}
              </button>
            </div>
            <div className="kc-field">
              <span>{selected ? (selected.title || selected.key) : t("control.api.noSelection")}</span>
              {(["continue", "interrupt", "stop"] as const).map((action) => (
                <button
                  key={action}
                  type="button"
                  className="kc-btn ghost"
                  disabled={m.crewApiBusy || !m.crewSelectedSlot}
                  onClick={() => m.runCrewApi(() => m.crewSlotAction(action))}
                >
                  {t(`control.api.${action}`)}
                </button>
              ))}
            </div>

            {(m.crewApiError || status.crew_api_last_error) && (
              <div className="error-banner" style={{ marginTop: 12 }}>
                {isIdentityFailure(m.crewApiError || (status.crew_api_last_error ?? "")) && <strong>{t("crew.identityFailed")} </strong>}
                {m.crewApiError || status.crew_api_last_error}
              </div>
            )}
          </Card>

          <Card title={t("control.keys.title")} info="help.control.keys">
            <p className="kc-muted kc-note">{t("control.keys.kbHint")}</p>
            {[1, 2, 3].map((slot) => {
              const defaultName = SLOT_DEFAULT_NAMES[slot - 1];
              return (
                <div key={slot} className="kc-field" style={{ marginTop: slot === 1 ? 0 : 8 }}>
                  <strong style={{ minWidth: 92 }}>{`${defaultName} · K${slot}`}</strong>
                  <input
                    className="binding-alias-input"
                    value={m.bindAliases[slot - 1] ?? ""}
                    maxLength={m.aliasMaxChars}
                    disabled={m.bindBusy}
                    aria-label={t("control.keys.alias", { name: defaultName })}
                    placeholder={t("control.keys.aliasPh", { name: defaultName })}
                    onChange={(e) => m.bindAliasDraft(slot, e.target.value)}
                    onBlur={(e) => m.runBind(() => m.bindAliasSave(slot, e.currentTarget.value))}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") e.currentTarget.blur();
                    }}
                    style={{ width: 120 }}
                  />
                  <select
                    value={m.bindSessions[slot - 1] ?? ""}
                    disabled={m.bindBusy || m.crewSlots.length === 0}
                    aria-label={t("control.keys.chat", { name: defaultName })}
                    onChange={(e) => {
                      const sessionId = e.target.value;
                      m.runBind(() => (sessionId ? m.bindSession(slot, m.binds[slot - 1] ?? 0, sessionId) : m.clearBoundSession(slot)));
                    }}
                    style={{ flex: 1 }}
                  >
                    <option value="">{t("control.keys.unbound")}</option>
                    {m.crewSlots.map((c) => (
                      <option key={c.key} value={c.key}>{c.title || c.key}</option>
                    ))}
                    {m.bindSessions[slot - 1] && !m.crewSlots.some((c) => c.key === m.bindSessions[slot - 1]) && (
                      <option value={m.bindSessions[slot - 1]}>{m.bindSessions[slot - 1]}</option>
                    )}
                  </select>
                </div>
              );
            })}
            {m.bindError && <div className="error-banner" style={{ marginTop: 10 }}>{m.bindError}</div>}
          </Card>

          <Card title={t("control.watched.title")} info="help.control.watched">
            <div className="kc-keymap-head" style={{ marginBottom: 8 }}>
              <button type="button" className={`kc-seg ${m.watchedView === "compact" ? "on" : ""}`} onClick={() => m.setWatchedView("compact")}>{t("control.watched.compact")}</button>
              <button type="button" className={`kc-seg ${m.watchedView === "detail" ? "on" : ""}`} onClick={() => m.setWatchedView("detail")}>{t("control.watched.detail")}</button>
            </div>
            <div className="kc-muted kc-note">
              {m.pinnedSession ? t("control.watched.pinned", { id: m.pinnedSession }) : t("control.watched.all")}
            </div>
            <div className="kc-stack" style={{ marginTop: 8 }}>
              {m.sortedKcStatuses.length === 0 && <div className="kc-muted kc-note">{t("control.watched.empty")}</div>}
              {m.sortedKcStatuses.map((s) => (
                <SessionRow key={s.session_id} m={m} s={s} />
              ))}
            </div>
          </Card>
        </>
      )}

      {mechanism === "legacy" && (
        <Card title={t("control.legacy.title")} info="help.control.legacy">
          <div className="kc-muted kc-note">{t("control.legacy.intro")}</div>

          <Step n={1} title={t("control.legacy.step1")} done={m.lcBound}>
            <div className="kc-field">
              <input value={m.lcTitle} placeholder={t("control.legacy.titlePh")} onChange={(e) => m.setLcTitle(e.target.value)} style={{ flex: 1 }} />
              <button type="button" className="kc-btn" disabled={m.lcBusy} onClick={() => m.runLc(m.lcBind)}>{t("control.legacy.bind")}</button>
            </div>
            <div className="kc-field">
              <input value={m.lcText} placeholder={t("control.legacy.textPh")} onChange={(e) => m.setLcText(e.target.value)} style={{ flex: 1 }} />
              <button type="button" className="kc-btn ghost" disabled={m.lcBusy} onClick={() => m.runLc(m.lcSendText)}>{t("control.legacy.sendText")}</button>
              <button type="button" className="kc-btn ghost" disabled={m.lcBusy} onClick={() => m.runLc(m.lcCommandBar)}>{t("control.legacy.cmdBar")}</button>
            </div>
            <div className="kc-field">
              {["enter", "esc", "up", "down", "left", "right"].map((name) => (
                <button key={name} type="button" className="kc-btn ghost" disabled={m.lcBusy} onClick={() => m.runLc(() => m.lcChord(name))}>
                  {name.toUpperCase()}
                </button>
              ))}
            </div>
            <div className="kc-field"><span>{t("control.legacy.focus")}</span><strong>{m.lcBound ? t("control.legacy.bound") : t("control.legacy.notBound")}{m.lcResult ? ` · ${m.lcResult}` : ""}</strong></div>
            {m.lcError && <div className="error-banner">{m.lcError}</div>}
          </Step>

          <Step n={2} title={t("control.legacy.step2")} done={m.binds.some((n) => n > 0)}>
            {[1, 2, 3].map((slot) => (
              <div key={slot} className="kc-field" style={{ marginTop: slot === 1 ? 0 : 6 }}>
                <strong style={{ minWidth: 92 }}>{`${SLOT_DEFAULT_NAMES[slot - 1]} · K${slot}`}</strong>
                <select
                  value={m.binds[slot - 1] ?? 0}
                  disabled={m.bindBusy}
                  aria-label={t("control.legacy.altN", { name: SLOT_DEFAULT_NAMES[slot - 1] })}
                  onChange={(e) => m.runBind(() => m.bindSet(slot, Number(e.target.value)))}
                >
                  <option value={0}>{t("control.legacy.noJump")}</option>
                  {[1, 2, 3, 4, 5, 6, 7, 8, 9].map((num) => (
                    <option key={num} value={num}>Alt+{num}</option>
                  ))}
                </select>
              </div>
            ))}
            <p className="kc-muted kc-note">{t("control.legacy.altHint")}</p>
          </Step>

          <Step n={3} title={t("control.legacy.step3")} done={m.hookStatus?.fully_installed === true && m.ssRunning}>
            <div className="kc-field">
              <span>{t("control.legacy.hookState")}</span>
              <StateBadge tone={m.hookStatus?.fully_installed ? "success" : "neutral"}>
                {!m.hookStatus
                  ? t("common.checking")
                  : m.hookStatus.fully_installed
                    ? t("control.legacy.hook.full")
                    : m.hookStatus.any_installed
                      ? t("control.legacy.hook.partial", { n: m.hookStatus.installed_events })
                      : t("control.legacy.hook.none")}
              </StateBadge>
            </div>
            <div className="kc-field">
              <button type="button" className="kc-btn ghost" disabled={m.ssBusy} onClick={() => m.runSs(m.hookRefresh)}>{t("common.refresh")}</button>
              <button type="button" className="kc-btn ghost" disabled={m.ssBusy} onClick={() => m.runSs(m.hookOpenFolder)}>{t("control.legacy.openFolder")}</button>
              <button type="button" className="kc-btn" disabled={m.ssBusy} onClick={() => m.runSs(m.hookInstall)}>{t("control.legacy.installHook")}</button>
              <button type="button" className="kc-btn ghost" disabled={m.ssBusy || !m.hookStatus?.any_installed} onClick={() => m.runSs(m.hookRemove)}>{t("control.legacy.removeHook")}</button>
            </div>
            <p className="kc-muted kc-note">{t("control.legacy.restartCrew")}</p>
            <div className="kc-field">
              <span>{t("control.legacy.listener")}</span>
              <StateBadge tone={m.ssRunning ? "success" : "neutral"}>{m.ssRunning ? t("control.legacy.listening") : t("control.legacy.stopped")}</StateBadge>
              <button type="button" className="kc-btn" disabled={m.ssBusy} onClick={() => m.runSs(m.ssRunning ? m.ssStop : m.ssStart)}>
                {m.ssRunning ? t("common.stop") : t("common.start")}
              </button>
            </div>
            {m.ssRunning && (
              <div className="kc-field" style={{ alignItems: "flex-start", flexDirection: "column" }}>
                <span>{t("control.legacy.token")}</span>
                <input readOnly value={m.ssToken} onFocus={(e) => e.currentTarget.select()} style={{ width: "100%" }} />
                <span>{t("control.legacy.hookCmd")}</span>
                <div className="kc-log" style={{ marginTop: 0 }}>{m.hookStatus?.command ?? '"<path-to-companion.exe>" --hook'}</div>
              </div>
            )}
            {m.ssError && <div className="error-banner">{m.ssError}</div>}
          </Step>

          <Step n={4} title={t("control.legacy.step4")} done={m.bindSessions.some((id) => id !== "")}>
            {[1, 2, 3].map((slot) => (
              <div key={slot} className="kc-field" style={{ marginTop: slot === 1 ? 0 : 6 }}>
                <strong style={{ minWidth: 92 }}>{`${SLOT_DEFAULT_NAMES[slot - 1]} · K${slot}`}</strong>
                <span className="kc-muted">{m.bindSessions[slot - 1] ? sessionDisplayName(m.bindSessions[slot - 1]) : t("control.keys.unbound")}</span>
                {m.bindSessions[slot - 1] && (
                  <button type="button" className="kc-btn ghost" disabled={m.bindBusy} onClick={() => m.runBind(() => m.clearBoundSession(slot))}>
                    {t("control.legacy.removeLink")}
                  </button>
                )}
              </div>
            ))}
          </Step>
        </Card>
      )}
    </div>
  );
}

function Metric({ label, value, ok }: { label: string; value: string; ok?: boolean }) {
  return (
    <div className="kc-metric">
      <span>{label}</span>
      <b className={ok ? "kc-ok" : ""}>{value}</b>
    </div>
  );
}

function Step({ n, title, done, children }: { n: number; title: string; done: boolean; children: ReactNode }) {
  return (
    <details className={`kc-step ${done ? "done" : ""}`} open>
      <summary>
        <span className="kc-step-num">{done ? "✓" : n}</span>
        <span>{title}</span>
        <span className="kc-muted" style={{ marginLeft: "auto" }}>{done ? "✓" : ""}</span>
      </summary>
      <div className="kc-step-body">{children}</div>
    </details>
  );
}

function SessionRow({ m, s }: { m: Model; s: KcStatus }) {
  const t = m.t;
  const isPinned = s.session_id === m.pinnedSession;
  const boundSlot = m.bindSessions.findIndex((id) => id === s.session_id) + 1;
  const draft = m.quickBindDraft?.sessionId === s.session_id ? m.quickBindDraft : null;
  return (
    <div className="kc-sess">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <button type="button" className="kc-seg" style={{ textAlign: "left" }}>
          {sessionDisplayName(s.session_id)}
        </button>
        <StateBadge tone={s.state.toLowerCase() === "running" ? "accent" : "neutral"}>{s.state.toUpperCase()}</StateBadge>
        {SLOT_DEFAULT_NAMES.map((name, i) => (
          <button
            key={name}
            type="button"
            className={m.bindSessions[i] === s.session_id ? "kc-btn" : "kc-btn ghost"}
            disabled={m.bindBusy}
            aria-label={t("control.keys.bindTo", { name })}
            onClick={() => m.startQuickBind(s.session_id, i + 1)}
          >
            {name}
          </button>
        ))}
        <button type="button" className={isPinned ? "kc-btn" : "kc-btn ghost"} disabled={m.ssBusy} onClick={() => m.runSs(() => (isPinned ? m.unpinSession() : m.pinSession(s.session_id)))}>
          {isPinned ? t("control.watch.stop") : t("control.watch.start")}
        </button>
      </div>
      {m.watchedView === "detail" ? (
        <div className="kc-muted kc-note">
          <code>{s.session_id}</code> · {s.event || t("control.watched.statusUpdate")} · {s.detail_text ?? s.text}
        </div>
      ) : (
        <div className="kc-muted kc-note">
          {boundSlot
            ? t("control.keys.boundTo", { name: SLOT_DEFAULT_NAMES[boundSlot - 1], alias: m.bindAliases[boundSlot - 1] || SLOT_DEFAULT_NAMES[boundSlot - 1] })
            : s.text}
        </div>
      )}
      {draft && (
        <div className="kc-field">
          <span>{t("control.keys.draft", { name: SLOT_DEFAULT_NAMES[draft.slot - 1] })}</span>
          <select value={draft.number} disabled={m.bindBusy} onChange={(e) => m.setQuickBindDraft({ ...draft, number: Number(e.target.value) })}>
            {[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((n) => (
              <option key={n} value={n}>{n === 0 ? t("control.legacy.noJump") : `Alt+${n}`}</option>
            ))}
          </select>
          <button type="button" className="kc-btn" disabled={m.bindBusy} onClick={() => m.runBind(m.confirmQuickBind)}>{t("common.save")}</button>
          {m.bindSessions[draft.slot - 1] && (
            <button type="button" className="kc-btn ghost" disabled={m.bindBusy} onClick={() => m.runBind(() => m.clearBoundSession(draft.slot))}>
              {t("control.legacy.removeLink")}
            </button>
          )}
          <button type="button" className="kc-btn ghost" disabled={m.bindBusy} onClick={() => m.setQuickBindDraft(null)}>{t("common.cancel")}</button>
        </div>
      )}
    </div>
  );
}
