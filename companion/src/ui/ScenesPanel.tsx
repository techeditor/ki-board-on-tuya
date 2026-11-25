/* FR-03 scene management.
 * Single column of cards: current scene + quick switch (with save / restore
 * when the keyboard changed it), the 4 keyboard favourite slots, the scene
 * list (switch / edit / copy / rename / delete / export, import), and the
 * editor. Presets are read-only: copy one to change it. The backend
 * (scene_sync.rs) owns validation and the keyboard sync; this page only calls
 * its commands and renders `scenes-changed`. */
import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { useI18n } from "../i18n";
import { tauriReady } from "../app/useCompanion";
import {
  HOME_LAYOUTS,
  IDLE_COLORS,
  KNOB_MODES,
  LED_AUTO_OFF_S,
  LED_POWERS,
  SCENE_ACTIONS,
  SCENE_KEYS,
  VOICE_MODES,
  gesturesOf,
  hasDouble,
  isPreset,
  joinAction,
  previewBackend,
  splitAction,
  type Scene,
  type SceneGesture,
  type SceneKey,
  type SceneView,
} from "../scenes/model";
import { Card, StateBadge } from "./primitives";

function call<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  return (tauriReady() ? invoke<T>(cmd, args) : previewBackend(cmd, args)) as Promise<T>;
}

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/** Scene state + commands, shared by the Advanced page and the Simple switch. */
export function useScenes() {
  const [view, setView] = useState<SceneView | null>(null);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  const refresh = () => call<SceneView>("scenes_list").then(setView);

  useEffect(() => {
    refresh().catch((e) => setError(errorText(e)));
    if (!tauriReady()) return;
    const offView = listen<SceneView>("scenes-changed", (e) => setView(e.payload));
    const offNotice = listen<string>("scene-notice", (e) => setNotice(e.payload));
    return () => {
      void offView.then((f) => f());
      void offNotice.then((f) => f());
    };
  }, []);

  async function run<T>(cmd: string, args?: Record<string, unknown>): Promise<T | undefined> {
    setError("");
    try {
      const out = await call<T>(cmd, args);
      if (!tauriReady()) await refresh(); // the backend pushes scenes-changed itself
      return out;
    } catch (e) {
      setError(errorText(e));
      return undefined;
    }
  }

  return { view, error, notice, setNotice, run };
}

export type ScenesModel = ReturnType<typeof useScenes>;

export function sceneName(view: SceneView | null, id: string | null | undefined): string {
  return view?.scenes.find((s) => s.id === id)?.name ?? "";
}

/** The Simple view's one-line quick switch. */
export function SceneQuickSwitch({ sc }: { sc: ScenesModel }) {
  const { t } = useI18n();
  const v = sc.view;
  if (!v) return null;
  return (
    <div className="kc-metric">
      <span>{t("scene.current")}</span>
      <select
        className="kc-scene-select"
        value={v.current ?? ""}
        disabled={v.switching !== null}
        aria-label={t("scene.switchTo")}
        onChange={(e) => void sc.run("scene_switch", { id: e.target.value })}
      >
        {!v.current && <option value="">{t("scene.none")}</option>}
        {v.scenes.map((s) => (
          <option key={s.id} value={s.id}>
            {s.name}
          </option>
        ))}
      </select>
      {v.dirty && <span className="kc-scene-note">{t("scene.dirtyShort")}</span>}
      {sc.error && <span className="kc-scene-note warn">{sc.error}</span>}
    </div>
  );
}

export function ScenesPanel({ sc }: { sc: ScenesModel }) {
  const { t } = useI18n();
  const v = sc.view;
  const [editing, setEditing] = useState<Scene | null>(null);
  const editorRef = useRef<HTMLDivElement>(null);

  if (!v) {
    return <Card title={t("scene.title")}>{sc.error ? <div className="error-banner">{sc.error}</div> : <span className="kc-muted">{t("common.checking")}</span>}</Card>;
  }

  const startEdit = (s: Scene) => {
    setEditing(structuredClone(s));
    window.setTimeout(() => editorRef.current?.scrollIntoView({ behavior: "smooth", block: "start" }), 0);
  };
  const createNew = async () => {
    const base = v.scenes.find((s) => s.id === v.presets[0]) ?? v.scenes[0];
    if (!base) return;
    const id = await sc.run<string>("scene_create", { scene: { ...structuredClone(base), name: t("scene.newName") } });
    const fresh = id ? (await call<SceneView>("scenes_list")).scenes.find((s) => s.id === id) : undefined;
    if (fresh) startEdit(fresh);
  };

  return (
    <div className="kc-stack">
      {v.store_error && <div className="error-banner">{t("scene.storeError", { error: v.store_error })}</div>}
      {sc.error && <div className="error-banner">{sc.error}</div>}
      {sc.notice && (
        <div className="kc-scene-notice" role="status">
          <span>{sc.notice}</span>
          <button type="button" className="kc-btn ghost" onClick={() => sc.setNotice("")}>{t("common.ok")}</button>
        </div>
      )}
      <CurrentCard sc={sc} v={v} />
      <FavoritesCard sc={sc} v={v} />
      <ListCard sc={sc} v={v} onEdit={startEdit} onNew={() => void createNew()} />
      <div ref={editorRef}>
        {editing && (
          <EditorCard
            key={editing.id}
            initial={editing}
            onCancel={() => setEditing(null)}
            onSave={async (s) => {
              const ok = (await sc.run("scene_update", { id: s.id, scene: s })) !== undefined;
              if (ok) setEditing(null);
            }}
          />
        )}
      </div>
    </div>
  );
}

function BoardBadge({ v }: { v: SceneView }) {
  const { t } = useI18n();
  if (v.board_synced) return <StateBadge tone="success">{t("scene.board.synced")}</StateBadge>;
  if (v.board_legacy) return <StateBadge tone="warning">{t("scene.board.legacy")}</StateBadge>;
  return <StateBadge>{t("scene.board.offline")}</StateBadge>;
}

function CurrentCard({ sc, v }: { sc: ScenesModel; v: SceneView }) {
  const { t } = useI18n();
  const [target, setTarget] = useState(v.current ?? v.scenes[0]?.id ?? "");
  const current = v.scenes.find((s) => s.id === v.current);
  const currentIsPreset = current ? isPreset(v, current.id) : false;
  return (
    <Card title={t("scene.current")} info="help.scene.current">
      <div className="kc-field" style={{ marginTop: 0 }}>
        <b className="kc-scene-name">{current?.name ?? t("scene.none")}</b>
        {v.dirty && <StateBadge tone="warning">{t("scene.dirty")}</StateBadge>}
        {v.switching && <StateBadge tone="accent">{t("scene.switching", { name: sceneName(v, v.switching) })}</StateBadge>}
        <span style={{ marginLeft: "auto" }}>
          <BoardBadge v={v} />
        </span>
      </div>
      <div className="kc-field">
        <select value={target} onChange={(e) => setTarget(e.target.value)} aria-label={t("scene.switchTo")} style={{ flex: 1 }}>
          {v.scenes.map((s) => (
            <option key={s.id} value={s.id}>
              {s.name}
            </option>
          ))}
        </select>
        {v.switching ? (
          <button type="button" className="kc-btn ghost" onClick={() => void sc.run("scene_cancel_switch")}>{t("common.cancel")}</button>
        ) : (
          <button type="button" className="kc-btn" disabled={!target || target === v.current} onClick={() => void sc.run("scene_switch", { id: target })}>
            {t("scene.switch")}
          </button>
        )}
      </div>
      {v.board_legacy && <p className="kc-muted kc-note">{t("scene.legacyHint")}</p>}
      {v.dirty && (
        <div className="kc-scene-dirty">
          <p className="kc-note" style={{ marginTop: 0 }}>{t("scene.dirtyHint")}</p>
          <div className="kc-field">
            <button
              type="button"
              className="kc-btn"
              disabled={currentIsPreset}
              title={currentIsPreset ? t("scene.presetReadOnly") : undefined}
              onClick={() => void sc.run("scene_save_live")}
            >
              {t("scene.saveLive")}
            </button>
            <button type="button" className="kc-btn ghost" onClick={() => void sc.run("scene_restore")}>{t("scene.restore")}</button>
          </div>
          {currentIsPreset && <p className="kc-muted kc-note">{t("scene.presetReadOnly")}</p>}
        </div>
      )}
    </Card>
  );
}

function FavoritesCard({ sc, v }: { sc: ScenesModel; v: SceneView }) {
  const { t } = useI18n();
  const [slots, setSlots] = useState<string[]>(() => [0, 1, 2, 3].map((i) => v.favorites[i] ?? ""));
  useEffect(() => setSlots([0, 1, 2, 3].map((i) => v.favorites[i] ?? "")), [v.favorites]);
  const picked = slots.filter(Boolean);
  const duplicate = new Set(picked).size !== picked.length;
  const changed = picked.join(",") !== v.favorites.join(",");
  return (
    <Card title={t("scene.favorites")} info="help.scene.favorites">
      <p className="kc-muted kc-note" style={{ marginTop: 0 }}>{t("scene.favoritesHint")}</p>
      {slots.map((id, i) => (
        <div key={i} className="kc-field">
          <span style={{ minWidth: 64 }}>{t("scene.slot", { n: i + 1 })}</span>
          <select
            value={id}
            style={{ flex: 1 }}
            aria-label={t("scene.slot", { n: i + 1 })}
            onChange={(e) => setSlots(slots.map((s, j) => (j === i ? e.target.value : s)))}
          >
            <option value="">{t("scene.slotEmpty")}</option>
            {v.scenes.map((s) => (
              <option key={s.id} value={s.id}>
                {s.name}
              </option>
            ))}
          </select>
        </div>
      ))}
      {duplicate && <div className="error-banner">{t("scene.favoritesDup")}</div>}
      <div className="kc-keymap-actions">
        <button type="button" className="kc-btn" disabled={!changed || duplicate} onClick={() => void sc.run("scene_set_favorites", { ids: picked })}>
          {t("common.save")}
        </button>
      </div>
    </Card>
  );
}

function ListCard({ sc, v, onEdit, onNew }: { sc: ScenesModel; v: SceneView; onEdit: (s: Scene) => void; onNew: () => void }) {
  const { t } = useI18n();
  const fileRef = useRef<HTMLInputElement>(null);
  const [renaming, setRenaming] = useState<{ id: string; name: string } | null>(null);
  const [armedDelete, setArmedDelete] = useState("");
  const [exported, setExported] = useState("");

  const doExport = async (ids: string[]) => {
    const path = await sc.run<string>("scene_export_file", { ids });
    if (path) setExported(path);
  };
  const doImport = async (file: File | undefined) => {
    if (!file) return;
    const text = await file.text();
    const ids = await sc.run<string[]>("scene_import", { text });
    if (ids) sc.setNotice(t("scene.imported", { n: ids.length }));
    if (fileRef.current) fileRef.current.value = "";
  };

  return (
    <Card title={t("scene.all")} info="help.scene.all">
      <div className="kc-field" style={{ marginTop: 0, flexWrap: "wrap" }}>
        <button type="button" className="kc-btn" onClick={onNew}>{t("scene.new")}</button>
        <button type="button" className="kc-btn ghost" onClick={() => fileRef.current?.click()}>{t("scene.import")}</button>
        <input ref={fileRef} type="file" accept=".json,application/json" hidden onChange={(e) => void doImport(e.target.files?.[0])} />
        <button type="button" className="kc-btn ghost" onClick={() => void doExport(v.scenes.map((s) => s.id))}>{t("scene.exportAll")}</button>
      </div>
      {exported && <p className="kc-muted kc-note">{t("scene.exportedTo", { path: exported })}</p>}

      <div className="kc-stack" style={{ marginTop: 10, gap: 6 }}>
        {v.scenes.map((s) => {
          const preset = isPreset(v, s.id);
          const fav = v.favorites.indexOf(s.id);
          const isCurrent = v.current === s.id;
          return (
            <div key={s.id} className={`kc-sess kc-scene-row ${isCurrent ? "selected" : ""}`}>
              <div className="kc-field" style={{ marginTop: 0, flexWrap: "wrap" }}>
                {renaming?.id === s.id ? (
                  <input
                    autoFocus
                    value={renaming.name}
                    maxLength={24}
                    aria-label={t("scene.rename")}
                    onChange={(e) => setRenaming({ id: s.id, name: e.target.value })}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") void sc.run("scene_rename", { id: s.id, name: renaming.name }).then(() => setRenaming(null));
                      if (e.key === "Escape") setRenaming(null);
                    }}
                    style={{ flex: 1 }}
                  />
                ) : (
                  <b className="kc-scene-name">{s.name}</b>
                )}
                {preset && <StateBadge>{t("scene.preset")}</StateBadge>}
                {fav >= 0 && <StateBadge tone="blue">{t("scene.favBadge", { n: fav + 1 })}</StateBadge>}
                {isCurrent && <StateBadge tone="success">{t("scene.currentBadge")}</StateBadge>}
              </div>
              <div className="kc-muted kc-note">{summary(t, s)}</div>
              <div className="kc-field kc-scene-actions">
                {renaming?.id === s.id ? (
                  <>
                    <button type="button" className="kc-btn" onClick={() => void sc.run("scene_rename", { id: s.id, name: renaming.name }).then(() => setRenaming(null))}>{t("common.save")}</button>
                    <button type="button" className="kc-btn ghost" onClick={() => setRenaming(null)}>{t("common.cancel")}</button>
                  </>
                ) : (
                  <>
                    <button type="button" className="kc-btn ghost" disabled={isCurrent || v.switching !== null} onClick={() => void sc.run("scene_switch", { id: s.id })}>{t("scene.switch")}</button>
                    {!preset && <button type="button" className="kc-btn ghost" onClick={() => onEdit(s)}>{t("scene.edit")}</button>}
                    {!preset && <button type="button" className="kc-btn ghost" onClick={() => setRenaming({ id: s.id, name: s.name })}>{t("scene.rename")}</button>}
                    <button type="button" className="kc-btn ghost" onClick={() => void sc.run("scene_duplicate", { id: s.id })}>{preset ? t("scene.copyToEdit") : t("scene.copy")}</button>
                    <button type="button" className="kc-btn ghost" onClick={() => void doExport([s.id])}>{t("scene.export")}</button>
                    {!preset && (
                      <button
                        type="button"
                        className={`kc-btn ghost ${armedDelete === s.id ? "kc-danger" : ""}`}
                        disabled={isCurrent}
                        title={isCurrent ? t("scene.deleteCurrent") : undefined}
                        onClick={() => {
                          if (armedDelete === s.id) {
                            setArmedDelete("");
                            void sc.run("scene_delete", { id: s.id });
                          } else {
                            setArmedDelete(s.id);
                            window.setTimeout(() => setArmedDelete((cur) => (cur === s.id ? "" : cur)), 3000);
                          }
                        }}
                      >
                        {armedDelete === s.id ? t("scene.deleteConfirm") : t("scene.delete")}
                      </button>
                    )}
                  </>
                )}
              </div>
            </div>
          );
        })}
        {v.board_only.map((b) => (
          <div key={`board-${b.slot}`} className="kc-sess kc-scene-row">
            <div className="kc-field" style={{ marginTop: 0 }}>
              <b className="kc-scene-name">{b.name}</b>
              <StateBadge tone="warning">{t("scene.boardOnly")}</StateBadge>
            </div>
            <div className="kc-field kc-scene-actions">
              <button type="button" className="kc-btn ghost" onClick={() => void sc.run("scene_import_from_board", { slot: b.slot })}>{t("scene.importFromBoard")}</button>
            </div>
          </div>
        ))}
      </div>
    </Card>
  );
}

type T = (key: string, vars?: Record<string, string | number>) => string;

function actionLabel(t: T, wire: string | undefined): string {
  const { family, n } = splitAction(wire);
  return t(`scene.action.${family}`, { n });
}

/** One line: what K1-K3 single / long do, the voice mode and the home page. */
function summary(t: T, s: Scene): string {
  const k1 = s.keys.k1;
  return t("scene.summary", {
    single: actionLabel(t, k1.single),
    long: actionLabel(t, k1.long),
    voice: t(`scene.voice.${s.voice_mode}`),
    home: t(`scene.home.${s.home}`),
  });
}

function EditorCard({ initial, onSave, onCancel }: { initial: Scene; onSave: (s: Scene) => Promise<void>; onCancel: () => void }) {
  const { t } = useI18n();
  const [s, setS] = useState<Scene>(initial);
  const [saving, setSaving] = useState(false);

  const setKey = (key: SceneKey, patch: Partial<Scene["keys"][SceneKey]>) =>
    setS({ ...s, keys: { ...s.keys, [key]: { ...s.keys[key], ...patch } } });
  const setIdle = (key: SceneKey, color: string) => {
    const idle = { ...(s.lights.idle ?? {}) };
    if (color === "off") delete idle[key];
    else idle[key] = color;
    setS({ ...s, lights: { ...s.lights, idle } });
  };
  const setKind = (key: SceneKey, kind: "normal" | "voice") =>
    setKey(key, kind === "voice" ? { kind: "voice", single: undefined, double: undefined, long: undefined } : { kind: undefined, single: "none", double: "none", long: key === "enc" ? undefined : "none" });

  return (
    <Card title={t("scene.editTitle", { name: initial.name })}>
      <label className="kc-field" style={{ marginTop: 0 }}>
        <span style={{ minWidth: 64 }}>{t("scene.name")}</span>
        <input value={s.name} maxLength={24} onChange={(e) => setS({ ...s, name: e.target.value })} style={{ flex: 1 }} />
      </label>

      <div className="kc-stack" style={{ marginTop: 10, gap: 8 }}>
        {SCENE_KEYS.map((key) => {
          const b = s.keys[key];
          const voice = b.kind === "voice";
          return (
            <div key={key} className="kc-keyblock">
              <div className="kc-inspector-head">
                <b className="kc-mono">{key === "enc" ? t("keymap.knob") : key.toUpperCase()}</b>
                <label className="kc-field" style={{ margin: 0 }}>
                  <span>{t("keymap.kindLabel")}</span>
                  <select value={voice ? "voice" : "normal"} onChange={(e) => setKind(key, e.target.value as "normal" | "voice")}>
                    <option value="normal">{t("keymap.kind.normal")}</option>
                    {key !== "enc" && <option value="voice">{t("keymap.kind.voice")}</option>}
                  </select>
                </label>
                <label className="kc-field" style={{ margin: 0 }}>
                  <span>{t("scene.idle")}</span>
                  <select value={s.lights.idle?.[key] ?? "off"} onChange={(e) => setIdle(key, e.target.value)}>
                    {IDLE_COLORS.map((c) => (
                      <option key={c} value={c}>{t(`scene.color.${c}`)}</option>
                    ))}
                  </select>
                </label>
              </div>
              {voice ? (
                <div className="kc-muted kc-note">{t("keymap.voiceHold")}</div>
              ) : (
                <>
                  {gesturesOf(key).map((g) => (
                    <GestureRow key={g} gesture={g} wire={b[g]} onChange={(w) => setKey(key, { [g]: w })} />
                  ))}
                  {hasDouble(b) && <p className="kc-scene-hint" role="note">{t("scene.doubleHint")}</p>}
                </>
              )}
            </div>
          );
        })}
      </div>

      <div className="kc-scene-grid">
        <Pick label={t("scene.knob")} value={s.knob} options={KNOB_MODES} prefix="scene.knob." onChange={(knob) => setS({ ...s, knob })} />
        <Pick label={t("scene.voiceMode")} value={s.voice_mode} options={VOICE_MODES} prefix="scene.voice." onChange={(voice_mode) => setS({ ...s, voice_mode })} />
        <Pick label={t("scene.homePage")} value={s.home} options={HOME_LAYOUTS} prefix="scene.home." onChange={(home) => setS({ ...s, home })} />
        <Pick label={t("scene.ledPower")} value={s.lights.power} options={LED_POWERS} prefix="scene.led." onChange={(power) => setS({ ...s, lights: { ...s.lights, power } })} />
        <label className="kc-field">
          <span>{t("scene.autoOff")}</span>
          <select value={s.lights.auto_off_s} disabled={s.lights.power !== "auto"} onChange={(e) => setS({ ...s, lights: { ...s.lights, auto_off_s: Number(e.target.value) } })}>
            {LED_AUTO_OFF_S.map((n) => (
              <option key={n} value={n}>{t(n < 60 ? "scene.seconds" : "scene.minutes", { n: n < 60 ? n : n / 60 })}</option>
            ))}
          </select>
        </label>
      </div>

      <div className="kc-keymap-actions">
        <button type="button" className="kc-btn ghost" onClick={onCancel}>{t("common.cancel")}</button>
        <button
          type="button"
          className="kc-btn"
          disabled={saving || !s.name.trim()}
          onClick={async () => {
            setSaving(true);
            await onSave({ ...s, name: s.name.trim() });
            setSaving(false);
          }}
        >
          {t("common.save")}
        </button>
      </div>
    </Card>
  );
}

function GestureRow({ gesture, wire, onChange }: { gesture: SceneGesture; wire: string | undefined; onChange: (w: string) => void }) {
  const { t } = useI18n();
  const { family, n } = splitAction(wire);
  const numbered = family === "chat" || family === "instance";
  return (
    <div className="kc-field">
      <span style={{ minWidth: 40 }}>{t(`keymap.gesture.${gesture}`)}</span>
      <select value={family} onChange={(e) => onChange(joinAction(e.target.value, n || 1))} style={{ flex: 1, minWidth: 0 }}>
        {SCENE_ACTIONS.map((a) => (
          <option key={a} value={a}>{t(`scene.action.${a}`, { n: "N" })}</option>
        ))}
      </select>
      {numbered && (
        <input className="kc-scene-num" type="number" min={1} max={9} value={n || 1} aria-label={t("scene.number")} onChange={(e) => onChange(joinAction(family, Number(e.target.value)))} />
      )}
    </div>
  );
}

function Pick({
  label,
  value,
  options,
  prefix,
  onChange,
}: {
  label: string;
  value: string;
  options: readonly string[];
  prefix: string;
  onChange: (v: string) => void;
}) {
  const { t } = useI18n();
  return (
    <label className="kc-field">
      <span>{label}</span>
      <select value={value} onChange={(e) => onChange(e.target.value)}>
        {options.map((o) => (
          <option key={o} value={o}>{t(prefix + o)}</option>
        ))}
      </select>
    </label>
  );
}
