/* Keymap stage 乙 — the action library + per-gesture binding model (design §5.5).
 * A key is either VOICE (hold-to-talk, no gesture rows) or NORMAL (click/double/
 * long, each bound to one action). The firmware owns the semantics; this file is
 * the UI's action vocabulary + the firmware-matching defaults + a default LED map
 * (color/effect derived from the action until per-row LED editing lands in 乙-全). */

export type KeySlot = "k1" | "k2" | "k3" | "k4" | "k5" | "k6" | "enc";
export const KEY_SLOTS: KeySlot[] = ["k1", "k2", "k3", "k4", "k5", "k6", "enc"];

export type KeyKind = "normal" | "voice";
export type Gesture = "click" | "double" | "long";
export const GESTURES: Gesture[] = ["click", "double", "long"];

/* An action's wire name. chatN/instanceN carry a 1..9 number. */
export type ActionWire = string; // "none" | "cancel_char" | "cancel_all" | "confirm" | "chat1".."chat9" | "instance1".."instance9"

/** Base (parameterless) actions a gesture can bind. chat/instance are added
 *  dynamically with a number picker. */
export const BASE_ACTIONS: { wire: ActionWire; labelKey: string }[] = [
  { wire: "none", labelKey: "action.none" },
  { wire: "chat", labelKey: "action.chat" }, // UI expands to chat<N>
  { wire: "instance", labelKey: "action.instance" }, // UI expands to instance<N>
  { wire: "cancel_char", labelKey: "action.cancelChar" },
  { wire: "cancel_all", labelKey: "action.cancelAll" },
  { wire: "confirm", labelKey: "action.confirm" },
];

/** Split a wire action into its family + optional number (chat3 -> {chat,3}). */
export function parseAction(wire: ActionWire): { family: string; n: number } {
  const m = /^(chat|instance)([1-9])$/.exec(wire);
  if (m) return { family: m[1], n: Number(m[2]) };
  return { family: wire || "none", n: 0 };
}

export function makeAction(family: string, n: number): ActionWire {
  if (family === "chat" || family === "instance") return `${family}${n >= 1 && n <= 9 ? n : 1}`;
  return family;
}

export type LedColor = "none" | "green" | "red" | "orange" | "blue" | "follow";
export type LedEffect = "none" | "single" | "double" | "breathing";

/** Default LED for an action (derived; per-row custom LED is 乙-全). The gesture
 *  it is bound to refines the effect (double gesture -> double blink). */
export function defaultLed(wire: ActionWire, gesture: Gesture): { color: LedColor; effect: LedEffect } {
  const { family } = parseAction(wire);
  switch (family) {
    case "chat":
    case "instance":
      return { color: "follow", effect: gesture === "double" ? "double" : "single" };
    case "cancel_char":
      return { color: "red", effect: "single" };
    case "cancel_all":
      return { color: "red", effect: "breathing" };
    case "confirm":
      return { color: "green", effect: "single" };
    default:
      return { color: "none", effect: "none" };
  }
}

export interface KeyBinding {
  kind: KeyKind;
  /** Action wire per gesture; "none" when unbound. Ignored when kind=voice. */
  actions: Record<Gesture, ActionWire>;
}

export type KeymapState = Record<KeySlot, KeyBinding>;

/** Firmware v0.3 defaults (must match ak_keymap_init_defaults). */
export function defaultKeymapState(): KeymapState {
  const normal = (click: ActionWire, dbl: ActionWire, long: ActionWire): KeyBinding => ({
    kind: "normal",
    actions: { click, double: dbl, long },
  });
  return {
    k1: normal("chat1", "instance1", "none"),
    k2: normal("chat2", "instance2", "none"),
    k3: normal("chat3", "instance3", "none"),
    k4: normal("cancel_char", "none", "cancel_all"),
    k5: { kind: "voice", actions: { click: "none", double: "none", long: "none" } },
    k6: normal("confirm", "none", "none"),
    enc: normal("none", "none", "none"),
  };
}

/** Flatten a KeymapState into the dotted (field,value) bindings set_keymap wants
 *  (full snapshot — every key's kind + its three gestures). */
export function toBindings(state: KeymapState): [string, string][] {
  const out: [string, string][] = [];
  for (const key of KEY_SLOTS) {
    const b = state[key];
    out.push([`${key}.kind`, b.kind]);
    if (b.kind === "normal") {
      for (const g of GESTURES) out.push([`${key}.${g}`, b.actions[g]]);
    }
  }
  return out;
}
