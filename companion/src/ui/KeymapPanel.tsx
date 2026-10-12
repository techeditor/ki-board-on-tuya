/* Keymap config panel — stage 甲 (design §5.5). Board-map default (option C):
 * a physical K1-K6 + EC11 grid with a live LED dot per key; clicking a key opens
 * an inspector showing that role's READ-ONLY (state×gesture)→(action,LED) rules
 * plus the one editable control, the role picker. An "Advanced — table view"
 * disclosure flips to the dense per-key table (option A). Both downlink the 7
 * roles via the set_keymap command. Behaviour is role-defined, not per-rule
 * editable (that is stage 乙). */
import { useState } from "react";

import { useI18n } from "../i18n";
import {
  DEFAULT_ROLES,
  KEY_SLOTS,
  ROLES,
  ROLE_RULES,
  gestureModeForRole,
  roleHasBindings,
  type KeySlot,
  type Role,
} from "../keymap/roleRules";
import { Card, InfoTip, StateBadge } from "./primitives";
import { LedDot } from "./LedDot";

/** Board rows for the physical layout: K1-K3 / K4-K6 / EC11 spanning. */
const BOARD_ROWS: KeySlot[][] = [
  ["k1", "k2", "k3"],
  ["k4", "k5", "k6"],
  ["enc"],
];

export interface KeymapState {
  roles: Record<KeySlot, Role>;
  chats: Record<KeySlot, number>; // k1-k3 click → chat N
  instances: Record<KeySlot, number>; // k1-k3 double → instance N
}

export function defaultKeymapState(): KeymapState {
  const chats: Record<string, number> = {};
  const instances: Record<string, number> = {};
  KEY_SLOTS.forEach((k, i) => {
    chats[k] = i < 3 ? i + 1 : 0;
    instances[k] = i < 3 ? i + 1 : 0;
  });
  return { roles: { ...DEFAULT_ROLES }, chats: chats as Record<KeySlot, number>, instances: instances as Record<KeySlot, number> };
}

/** The live board LED color for a key given its role (idle snapshot used on the
 * config surface; a real-time feed can override this when the board reports). */
function idleLed(role: Role): { color: Parameters<typeof LedDot>[0]["color"]; effect: Parameters<typeof LedDot>[0]["effect"] } {
  // On the config surface we show the steady idle color; pending-confirm colors
  // (red/green) light only when the board is actually in that state.
  return role === "none" ? { color: "none", effect: "none" } : { color: "green", effect: "single" };
}

export function KeymapPanel({
  value,
  onChange,
  onApply,
}: {
  value: KeymapState;
  onChange: (next: KeymapState) => void;
  onApply: (roles: Role[]) => void;
}) {
  const { t } = useI18n();
  const [selected, setSelected] = useState<KeySlot>("k1");
  const [advanced, setAdvanced] = useState(false);

  const setRole = (key: KeySlot, role: Role) => onChange({ ...value, roles: { ...value.roles, [key]: role } });
  const setChat = (key: KeySlot, n: number) => onChange({ ...value, chats: { ...value.chats, [key]: n } });
  const setInstance = (key: KeySlot, n: number) => onChange({ ...value, instances: { ...value.instances, [key]: n } });

  const applyRoles = () => onApply(KEY_SLOTS.map((k) => value.roles[k]));
  const resetDefaults = () => onChange(defaultKeymapState());

  return (
    <Card title={t("keymap.title")} info="help.keymap.panel">
      <div className="kc-keymap-head">
        <button
          type="button"
          className={`kc-seg ${!advanced ? "on" : ""}`}
          onClick={() => setAdvanced(false)}
        >
          {t("keymap.boardView")}
        </button>
        <button
          type="button"
          className={`kc-seg ${advanced ? "on" : ""}`}
          onClick={() => setAdvanced(true)}
        >
          {t("keymap.tableView")}
        </button>
      </div>

      {!advanced ? (
        <BoardMap
          value={value}
          selected={selected}
          onSelect={setSelected}
          setRole={setRole}
          setChat={setChat}
          setInstance={setInstance}
        />
      ) : (
        <KeymapTable value={value} setRole={setRole} setChat={setChat} setInstance={setInstance} />
      )}

      <div className="kc-keymap-actions">
        <button type="button" className="kc-btn ghost" onClick={resetDefaults}>
          {t("common.reset")}
        </button>
        <button type="button" className="kc-btn" onClick={applyRoles}>
          {t("common.apply")}
        </button>
      </div>
    </Card>
  );
}

function BoardMap({
  value,
  selected,
  onSelect,
  setRole,
  setChat,
  setInstance,
}: {
  value: KeymapState;
  selected: KeySlot;
  onSelect: (k: KeySlot) => void;
  setRole: (k: KeySlot, r: Role) => void;
  setChat: (k: KeySlot, n: number) => void;
  setInstance: (k: KeySlot, n: number) => void;
}) {
  const { t } = useI18n();
  const role = value.roles[selected];
  const rules = ROLE_RULES[role];

  return (
    <>
      <div className="kc-board">
        {BOARD_ROWS.map((row, ri) => (
          <div key={ri} className={`kc-board-row ${row[0] === "enc" ? "enc" : ""}`}>
            {row.map((k) => {
              const led = idleLed(value.roles[k]);
              return (
                <button
                  key={k}
                  type="button"
                  className={`kc-bkey ${selected === k ? "sel" : ""}`}
                  onClick={() => onSelect(k)}
                >
                  <span className="kc-bkey-id">{k === "enc" ? "EC11" : k.toUpperCase()}</span>
                  <span className="kc-bkey-role">
                    <LedDot color={led.color} effect={led.effect} />
                    {t(`keymap.role.${value.roles[k]}`)}
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
          <b>{t("keymap.inspector", { key: selected === "enc" ? "EC11" : selected.toUpperCase() })}</b>
          <StateBadge tone="accent">
            {gestureModeForRole(role) === "single_double" ? t("keymap.gesture.singleDouble") : t("keymap.gesture.edge")}
          </StateBadge>
        </div>

        <label className="kc-field">
          <span>{t("keymap.role")}</span>
          <select value={role} onChange={(e) => setRole(selected, e.target.value as Role)}>
            {ROLES.map((r) => (
              <option key={r} value={r}>
                {t(`keymap.role.${r}`)}
              </option>
            ))}
          </select>
        </label>

        {roleHasBindings(role) && (
          <div className="kc-bindings">
            <label className="kc-field">
              <span>{t("keymap.chatNumber")}</span>
              <input type="number" min={1} max={9} value={value.chats[selected] || 1} onChange={(e) => setChat(selected, Number(e.target.value))} />
            </label>
            <label className="kc-field">
              <span>
                {t("keymap.instanceNumber")}
                <InfoTip k="help.keymap.instance" />
              </span>
              <input type="number" min={1} max={9} value={value.instances[selected] || 1} onChange={(e) => setInstance(selected, Number(e.target.value))} />
            </label>
          </div>
        )}

        <table className="kc-rules">
          <thead>
            <tr>
              <th>{t("keymap.state.any").length ? t("keymap.gesture") : ""}</th>
              <th>{t("keymap.role")}</th>
              <th>LED</th>
            </tr>
          </thead>
          <tbody>
            {rules.map((rule, i) => (
              <tr key={i}>
                <td className="kc-mono">{t(`keymap.state.${rule.state === "pending-confirm" ? "pending" : rule.state}`)} · {rule.gesture}</td>
                <td>{t(rule.actionKey)}</td>
                <td>
                  <LedDot color={rule.led.color} effect={rule.led.effect} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}

function KeymapTable({
  value,
  setRole,
  setChat,
  setInstance,
}: {
  value: KeymapState;
  setRole: (k: KeySlot, r: Role) => void;
  setChat: (k: KeySlot, n: number) => void;
  setInstance: (k: KeySlot, n: number) => void;
}) {
  const { t } = useI18n();
  return (
    <table className="kc-keytable">
      <thead>
        <tr>
          <th>{t("keymap.title").slice(0, 2)}</th>
          <th>{t("keymap.role")}</th>
          <th>{t("keymap.gesture")}</th>
          <th>{t("keymap.binding")}</th>
        </tr>
      </thead>
      <tbody>
        {KEY_SLOTS.map((k) => {
          const role = value.roles[k];
          return (
            <tr key={k}>
              <td className="kc-mono">{k === "enc" ? "EC11" : k.toUpperCase()}</td>
              <td>
                <select value={role} onChange={(e) => setRole(k, e.target.value as Role)}>
                  {ROLES.map((r) => (
                    <option key={r} value={r}>
                      {t(`keymap.role.${r}`)}
                    </option>
                  ))}
                </select>
              </td>
              <td>
                <StateBadge tone={gestureModeForRole(role) === "single_double" ? "accent" : "blue"}>
                  {gestureModeForRole(role) === "single_double" ? t("keymap.gesture.singleDouble") : t("keymap.gesture.edge")}
                </StateBadge>
              </td>
              <td className="kc-mono">
                {roleHasBindings(role) ? (
                  <span className="kc-inline-binds">
                    <input type="number" min={1} max={9} value={value.chats[k] || 1} onChange={(e) => setChat(k, Number(e.target.value))} />
                    <input type="number" min={1} max={9} value={value.instances[k] || 1} onChange={(e) => setInstance(k, Number(e.target.value))} />
                  </span>
                ) : (
                  <span className="kc-muted">—</span>
                )}
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

function LedLegend() {
  const { t } = useI18n();
  return (
    <div className="kc-legend">
      <span><LedDot color="green" effect="none" /> {t("led.color.green")}</span>
      <span><LedDot color="red" effect="none" /> {t("led.color.red")}</span>
      <span><LedDot color="orange" effect="none" /> {t("led.color.orange")}</span>
      <span><LedDot color="blue" effect="none" /> {t("led.color.blue")}</span>
    </div>
  );
}
