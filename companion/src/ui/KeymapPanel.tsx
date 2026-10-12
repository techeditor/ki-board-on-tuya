/* Keymap config panel — stage 乙 (design §5.5). Board-map default (option C):
 * a physical K1-K6 + EC11 grid with a live LED dot per key; clicking a key opens
 * an inspector. A VOICE key's inspector just says hold-to-talk; a NORMAL key's
 * inspector shows three gesture ROWS (click/double/long), each with an action
 * picker (+ a 1-9 number for chat/instance). An "Advanced — table view"
 * disclosure shows every key's rows at once. Apply downlinks the full binding
 * snapshot via set_keymap. */
import { useState } from "react";

import { useI18n } from "../i18n";
import {
  BASE_ACTIONS,
  GESTURES,
  KEY_SLOTS,
  defaultKeymapState,
  defaultLed,
  makeAction,
  parseAction,
  type Gesture,
  type KeySlot,
  type KeymapState,
} from "../keymap/actions";
import { Card, InfoTip } from "./primitives";
import { LedDot } from "./LedDot";

export { defaultKeymapState };
export type { KeymapState };

const BOARD_ROWS: KeySlot[][] = [
  ["k1", "k2", "k3"],
  ["k4", "k5", "k6"],
  ["enc"],
];

export function KeymapPanel({
  value,
  onChange,
  onApply,
}: {
  value: KeymapState;
  onChange: (next: KeymapState) => void;
  onApply: (state: KeymapState) => void;
}) {
  const { t } = useI18n();
  const [selected, setSelected] = useState<KeySlot>("k1");
  const [advanced, setAdvanced] = useState(false);
  const [applied, setApplied] = useState(false);

  const handleApply = () => {
    onApply(value);
    setApplied(true);
    window.setTimeout(() => setApplied(false), 2500);
  };

  const setKind = (key: KeySlot, kind: "normal" | "voice") =>
    onChange({ ...value, [key]: { ...value[key], kind } });
  const setAction = (key: KeySlot, g: Gesture, wire: string) =>
    onChange({ ...value, [key]: { ...value[key], kind: "normal", actions: { ...value[key].actions, [g]: wire } } });

  return (
    <Card title={t("keymap.title")} info="help.keymap.panel">
      <div className="kc-keymap-head">
        <button type="button" className={`kc-seg ${!advanced ? "on" : ""}`} onClick={() => setAdvanced(false)}>
          {t("keymap.boardView")}
        </button>
        <button type="button" className={`kc-seg ${advanced ? "on" : ""}`} onClick={() => setAdvanced(true)}>
          {t("keymap.tableView")}
        </button>
      </div>

      {!advanced ? (
        <BoardMap value={value} selected={selected} onSelect={setSelected} setKind={setKind} setAction={setAction} />
      ) : (
        <KeymapTable value={value} setKind={setKind} setAction={setAction} />
      )}

      <div className="kc-keymap-actions">
        {applied && <span className="kc-applied">{t("keymap.applied")}</span>}
        <button type="button" className="kc-btn ghost" onClick={() => onChange(defaultKeymapState())}>
          {t("common.reset")}
        </button>
        <button type="button" className="kc-btn" onClick={handleApply}>
          {t("common.apply")}
        </button>
      </div>
    </Card>
  );
}

/** The action picker for one gesture row: a family select + (for chat/instance)
 *  a 1-9 number, with the derived LED dot. */
function ActionRow({
  gesture,
  wire,
  onChange,
}: {
  gesture: Gesture;
  wire: string;
  onChange: (wire: string) => void;
}) {
  const { t } = useI18n();
  const { family, n } = parseAction(wire);
  const led = defaultLed(wire, gesture);
  const needsNumber = family === "chat" || family === "instance";
  const gestureLabel = t(`keymap.gesture.${gesture}`);
  // Plain-language "what this does" line (H2): resolve the binding to a sentence.
  const resolvedKey =
    family === "chat"
      ? "keymap.resolved.chat"
      : family === "instance"
        ? "keymap.resolved.instance"
        : family === "cancel_char"
          ? "keymap.resolved.cancelChar"
          : family === "cancel_all"
            ? "keymap.resolved.cancelAll"
            : family === "confirm"
              ? "keymap.resolved.confirm"
              : "keymap.resolved.none";
  const resolved = t(resolvedKey, { gesture: gestureLabel, n: n || 1 });
  return (
    <tr>
      <td className="kc-mono">{gestureLabel}</td>
      <td>
        <div className="kc-actionpick">
          <select value={family} onChange={(e) => onChange(makeAction(e.target.value, n || 1))}>
            {BASE_ACTIONS.map((a) => (
              <option key={a.wire} value={a.wire}>
                {t(a.labelKey)}
              </option>
            ))}
          </select>
          {needsNumber && (
            <label className="kc-numlabel">
              {t(family === "chat" ? "keymap.chatNum" : "keymap.instanceNum")}
              <input
                type="number"
                min={1}
                max={9}
                value={n || 1}
                onChange={(e) => onChange(makeAction(family, Number(e.target.value)))}
              />
            </label>
          )}
        </div>
        <div className="kc-resolved">{resolved}</div>
      </td>
      <td>
        <LedDot color={led.color} effect={led.effect} />
      </td>
    </tr>
  );
}

function BoardMap({
  value,
  selected,
  onSelect,
  setKind,
  setAction,
}: {
  value: KeymapState;
  selected: KeySlot;
  onSelect: (k: KeySlot) => void;
  setKind: (k: KeySlot, kind: "normal" | "voice") => void;
  setAction: (k: KeySlot, g: Gesture, wire: string) => void;
}) {
  const { t } = useI18n();
  const b = value[selected];

  const keyLabel = (k: KeySlot) => (k === "enc" ? t("keymap.knob") : k.toUpperCase());
  const kindDot = (k: KeySlot) =>
    value[k].kind === "voice" ? { color: "blue" as const, effect: "none" as const } : { color: "green" as const, effect: "single" as const };

  return (
    <>
      <p className="kc-muted" style={{ fontSize: 12, margin: "0 0 10px" }}>{t("keymap.tapHint")}</p>
      <div className="kc-board">
        {BOARD_ROWS.map((row, ri) => (
          <div key={ri} className={`kc-board-row ${row[0] === "enc" ? "enc" : ""}`}>
            {row.map((k) => {
              const led = kindDot(k);
              return (
                <button key={k} type="button" className={`kc-bkey ${selected === k ? "sel" : ""}`} onClick={() => onSelect(k)}>
                  <span className="kc-bkey-id">{keyLabel(k)}</span>
                  <span className="kc-bkey-role">
                    <LedDot color={led.color} effect={led.effect} />
                    {value[k].kind === "voice" ? t("keymap.kind.voice") : t("keymap.kind.normal")}
                  </span>
                </button>
              );
            })}
          </div>
        ))}
      </div>

      <LedLegend />

      <div className="kc-inspector">
        <div className="kc-inspector-head">
          <b>{t("keymap.inspector", { key: keyLabel(selected) })}</b>
          <label className="kc-field" style={{ margin: 0 }}>
            <span>{t("keymap.kindLabel")}</span>
            <select value={b.kind} onChange={(e) => setKind(selected, e.target.value as "normal" | "voice")}>
              <option value="normal">{t("keymap.kind.normal")}</option>
              <option value="voice">{t("keymap.kind.voice")}</option>
            </select>
          </label>
        </div>

        {b.kind === "voice" ? (
          <p className="kc-muted" style={{ fontSize: 12, marginTop: 10 }}>
            {t("keymap.voiceHold")}
          </p>
        ) : (
          <table className="kc-rules">
            <thead>
              <tr>
                <th>{t("keymap.gesture")}</th>
                <th>{t("keymap.action")}</th>
                <th>LED</th>
              </tr>
            </thead>
            <tbody>
              {GESTURES.map((g) => (
                <ActionRow key={g} gesture={g} wire={b.actions[g]} onChange={(w) => setAction(selected, g, w)} />
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );
}

function KeymapTable({
  value,
  setKind,
  setAction,
}: {
  value: KeymapState;
  setKind: (k: KeySlot, kind: "normal" | "voice") => void;
  setAction: (k: KeySlot, g: Gesture, wire: string) => void;
}) {
  const { t } = useI18n();
  return (
    <div className="kc-stack">
      {KEY_SLOTS.map((k) => {
        const b = value[k];
        return (
          <div key={k} className="kc-keyblock">
            <div className="kc-inspector-head">
              <b className="kc-mono">{k === "enc" ? "EC11" : k.toUpperCase()}</b>
              <label className="kc-field" style={{ margin: 0 }}>
                <span>{t("keymap.kindLabel")}</span>
                <select value={b.kind} onChange={(e) => setKind(k, e.target.value as "normal" | "voice")}>
                  <option value="normal">{t("keymap.kind.normal")}</option>
                  <option value="voice">{t("keymap.kind.voice")}</option>
                </select>
              </label>
            </div>
            {b.kind === "voice" ? (
              <div className="kc-muted" style={{ fontSize: 12 }}>{t("keymap.voiceHold")}</div>
            ) : (
              <table className="kc-rules">
                <tbody>
                  {GESTURES.map((g) => (
                    <ActionRow key={g} gesture={g} wire={b.actions[g]} onChange={(w) => setAction(k, g, w)} />
                  ))}
                </tbody>
              </table>
            )}
          </div>
        );
      })}
    </div>
  );
}

function LedLegend() {
  const { t } = useI18n();
  return (
    <div className="kc-legend">
      <span className="kc-legend-title">{t("led.legend")}<InfoTip k="help.led" /></span>
      <span><LedDot color="green" effect="none" /> {t("led.color.green")}</span>
      <span><LedDot color="red" effect="none" /> {t("led.color.red")}</span>
      <span><LedDot color="orange" effect="none" /> {t("led.color.orange")}</span>
      <span><LedDot color="blue" effect="none" /> {t("led.color.blue")}</span>
    </div>
  );
}
