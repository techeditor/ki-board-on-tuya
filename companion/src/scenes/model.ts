/* FR-03 scenes — UI model. Mirrors companion/src-tauri/src/scene.rs (the
 * backend validates everything again; these lists only drive the pickers).
 * The browser preview (no Tauri backend) uses `previewBackend` so the page can
 * be seen and screenshotted. */

export type SceneKey = "k1" | "k2" | "k3" | "k4" | "k5" | "k6" | "enc";
export const SCENE_KEYS: SceneKey[] = ["k1", "k2", "k3", "k4", "k5", "k6", "enc"];
export type SceneGesture = "single" | "double" | "long";

export interface SceneKeyBinding {
  kind?: "normal" | "voice";
  single?: string;
  double?: string;
  long?: string;
}

export interface Scene {
  id: string;
  name: string;
  rev: number;
  keys: Record<SceneKey, SceneKeyBinding>;
  knob: string;
  voice_mode: string;
  home: string;
  lights: { power: string; auto_off_s: number; idle?: Partial<Record<SceneKey, string>> };
}

export interface SceneView {
  scenes: Scene[];
  presets: string[];
  favorites: string[];
  current: string | null;
  board_synced: boolean;
  board_legacy: boolean;
  dirty: boolean;
  switching: string | null;
  board_only: { slot: number; name: string }[];
  store_error: string | null;
}

/* Action families for the picker; chat / instance take a number 1..9. Same
 * allowlist as scene.rs (FIXED_ACTIONS + chatN / instanceN + SHORTCUTS). */
export const SCENE_ACTIONS = [
  "none",
  "chat",
  "instance",
  "cancel_char",
  "cancel_all",
  "confirm",
  "model_menu",
  "model_toggle",
  "scene_menu",
  "key_enter",
  "key_esc",
  "key_up",
  "key_down",
  "key_left",
  "key_right",
  "key_backspace",
  "key_delete_word",
  "key_clear_input",
  "key_command_bar",
  "key_prev_chat",
  "key_next_chat",
  "key_focus_input",
  "key_new_chat",
] as const;

export const KNOB_MODES = ["arrows", "chats", "none"] as const;
export const VOICE_MODES = ["asr", "chat"] as const;
export const HOME_LAYOUTS = ["sessions", "keys"] as const;
export const LED_POWERS = ["auto", "on", "off"] as const;
export const LED_AUTO_OFF_S = [30, 60, 300, 900] as const;
/** FR-03 D3: red and yellow keep their FR-09 meanings. */
export const IDLE_COLORS = ["off", "blue", "green", "purple"] as const;

export function splitAction(wire: string | undefined): { family: string; n: number } {
  const m = /^(chat|instance)([1-9])$/.exec(wire ?? "");
  if (m) return { family: m[1], n: Number(m[2]) };
  return { family: wire || "none", n: 0 };
}

export function joinAction(family: string, n: number): string {
  return family === "chat" || family === "instance" ? `${family}${n >= 1 && n <= 9 ? n : 1}` : family;
}

export function gesturesOf(key: SceneKey): SceneGesture[] {
  return key === "enc" ? ["single", "double"] : ["single", "double", "long"];
}

/** A bound double click makes this key's single click wait for the double
 *  window (~0.25 s). Shown as a non-blocking hint, for every key. */
export function hasDouble(b: SceneKeyBinding): boolean {
  return b.kind !== "voice" && (b.double ?? "none") !== "none";
}

export function isPreset(view: SceneView, id: string): boolean {
  return view.presets.includes(id);
}

/* ── Browser preview backend ───────────────────────────────────────────── */

const k = (single: string, long = "none", double = "none"): SceneKeyBinding => ({ single, double, long });
const voice: SceneKeyBinding = { kind: "voice" };
const codingKeys = (): Record<SceneKey, SceneKeyBinding> => ({
  k1: k("chat1", "instance1"),
  k2: k("chat2", "instance2"),
  k3: k("chat3", "instance3"),
  k4: k("cancel_char", "cancel_all"),
  k5: voice,
  k6: k("confirm", "model_menu", "model_toggle"),
  enc: { double: "scene_menu" },
});

function previewView(): SceneView {
  const lights = (idle: Partial<Record<SceneKey, string>> = {}) => ({ power: "auto", auto_off_s: 60, idle });
  return {
    scenes: [
      { id: "preset01", name: "编程", rev: 0, keys: codingKeys(), knob: "arrows", voice_mode: "asr", home: "sessions", lights: lights() },
      { id: "preset02", name: "语音对话", rev: 0, keys: codingKeys(), knob: "arrows", voice_mode: "chat", home: "sessions", lights: lights({ k5: "purple" }) },
      {
        id: "preset03",
        name: "导航",
        rev: 0,
        keys: { ...codingKeys(), k1: k("chat1", "key_prev_chat"), k2: k("chat2", "key_new_chat"), k3: k("chat3", "key_next_chat"), k4: k("key_esc", "cancel_all") },
        knob: "chats",
        voice_mode: "asr",
        home: "keys",
        lights: lights({ k1: "blue", k2: "blue", k3: "blue" }),
      },
      { id: "u7k2m9qa", name: "Review", rev: 3, keys: { ...codingKeys(), k2: k("chat2", "instance2", "key_command_bar") }, knob: "arrows", voice_mode: "asr", home: "keys", lights: lights() },
    ],
    presets: ["preset01", "preset02", "preset03"],
    favorites: ["preset01", "u7k2m9qa"],
    current: "u7k2m9qa",
    board_synced: true,
    board_legacy: false,
    dirty: true,
    switching: null,
    board_only: [{ slot: 4, name: "Other PC" }],
    store_error: null,
  };
}

let previewState: SceneView | null = null;

/** Minimal in-memory stand-in for the scene commands (preview only). */
export async function previewBackend(cmd: string, args: Record<string, unknown> = {}): Promise<unknown> {
  const v = (previewState ??= previewView());
  const id = args.id as string;
  const find = () => v.scenes.find((s) => s.id === id);
  const newId = () => Math.random().toString(36).slice(2, 10).padEnd(8, "0");
  switch (cmd) {
    case "scenes_list":
      return v;
    case "scene_duplicate": {
      const s = find();
      if (!s) throw new Error("no such scene");
      const copy = { ...structuredClone(s), id: newId(), rev: 0, name: `${s.name} (2)` };
      v.scenes.push(copy);
      return copy.id;
    }
    case "scene_create": {
      const s = { ...(structuredClone(args.scene) as Scene), id: newId(), rev: 0 };
      v.scenes.push(s);
      return s.id;
    }
    case "scene_update": {
      const i = v.scenes.findIndex((s) => s.id === id);
      if (i < 0 || v.presets.includes(id)) throw new Error("presets are read-only; copy it first");
      v.scenes[i] = { ...(args.scene as Scene), id, rev: v.scenes[i].rev + 1 };
      return null;
    }
    case "scene_rename": {
      const s = find();
      if (!s || v.presets.includes(id)) throw new Error("presets are read-only; copy it first");
      s.name = String(args.name).trim();
      return null;
    }
    case "scene_delete":
      if (v.presets.includes(id)) throw new Error("presets cannot be deleted");
      if (v.current === id) throw new Error("switch to another scene before deleting this one");
      v.scenes = v.scenes.filter((s) => s.id !== id);
      v.favorites = v.favorites.filter((f) => f !== id);
      return null;
    case "scene_set_favorites":
      v.favorites = args.ids as string[];
      return null;
    case "scene_switch":
      v.current = id;
      v.dirty = false;
      return null;
    case "scene_restore":
    case "scene_save_live":
      v.dirty = false;
      return null;
    case "scene_export_file":
      return "~/.ak-companion/scene-exports/scenes-preview.json";
    case "scene_import":
      throw new Error("import needs the desktop app");
    default:
      return null;
  }
}
