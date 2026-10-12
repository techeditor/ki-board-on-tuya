/* Role → (state × gesture) → (action, LED) rule matrix (design §5.5.2).
 *
 * Stage 甲: a key's whole behaviour is determined by its ROLE; this table is
 * the READ-ONLY data the keymap inspector renders for the picked role. It is
 * NOT fetched (it mirrors the shipped firmware's fixed role semantics) and NOT
 * per-rule editable — changing behaviour means changing the role. Stage 乙
 * (future per-rule editing) seeds its defaults from this same table. */

export type Role = "none" | "chat" | "voice" | "cancel" | "confirm";

export const ROLES: Role[] = ["none", "chat", "voice", "cancel", "confirm"];

/** The 7 mappable inputs, in keymap slot order (k1..k6, then enc). */
export const KEY_SLOTS = ["k1", "k2", "k3", "k4", "k5", "k6", "enc"] as const;
export type KeySlot = (typeof KEY_SLOTS)[number];

/** Firmware v0.3 default roles (must match ak_keymap_init_defaults). */
export const DEFAULT_ROLES: Record<KeySlot, Role> = {
  k1: "chat",
  k2: "chat",
  k3: "chat",
  k4: "cancel",
  k5: "voice",
  k6: "confirm",
  enc: "none",
};

/** Gesture-detection mode a role implies (derived, read-only; matches
 *  ak_keymap_gesture_for_role: chat → single/double, else edge). */
export type GestureMode = "single_double" | "edge";
export function gestureModeForRole(role: Role): GestureMode {
  return role === "chat" ? "single_double" : "edge";
}

/** An LED hint: the semantic color token + the blink effect (design §0.3/§0.5). */
export type LedColor = "none" | "green" | "red" | "orange" | "blue" | "follow";
export type LedEffect = "none" | "single" | "double" | "breathing";

export interface Rule {
  /** When this rule applies: "any" or a named board state. */
  state: "any" | "idle" | "pending-confirm";
  gesture: "click" | "double" | "long" | "hold" | "none";
  /** Human-facing action summary (i18n key resolved by the caller via t()). */
  actionKey: string;
  led: { color: LedColor; effect: LedEffect };
}

/** The read-only rule rows for each role (design §5.5.2). */
export const ROLE_RULES: Record<Role, Rule[]> = {
  none: [{ state: "any", gesture: "none", actionKey: "keymap.rule.none", led: { color: "none", effect: "none" } }],
  chat: [
    { state: "any", gesture: "click", actionKey: "keymap.rule.chatClick", led: { color: "follow", effect: "single" } },
    { state: "any", gesture: "double", actionKey: "keymap.rule.chatDouble", led: { color: "follow", effect: "double" } },
  ],
  voice: [{ state: "idle", gesture: "hold", actionKey: "keymap.rule.voiceHold", led: { color: "none", effect: "none" } }],
  cancel: [
    { state: "pending-confirm", gesture: "click", actionKey: "keymap.rule.cancelClick", led: { color: "red", effect: "single" } },
    { state: "pending-confirm", gesture: "long", actionKey: "keymap.rule.cancelLong", led: { color: "red", effect: "breathing" } },
  ],
  confirm: [
    { state: "pending-confirm", gesture: "click", actionKey: "keymap.rule.confirmClick", led: { color: "green", effect: "single" } },
  ],
};

/** Does this role expose chat#/instance# bindings (chat keys only)? */
export function roleHasBindings(role: Role): boolean {
  return role === "chat";
}
