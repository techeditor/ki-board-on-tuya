/* A single LED dot that mirrors the board's light for one key: color = state,
 * effect = blink pattern (design §0.3/§0.5). Colors are semantic tokens so the
 * dot stays readable in every theme. Honors reduced-motion. */
import type { LedColor, LedEffect } from "../keymap/actions";

const COLOR_VAR: Record<LedColor, string> = {
  none: "var(--border-strong)",
  green: "var(--success)",
  red: "var(--danger)",
  orange: "var(--warning)",
  blue: "var(--blue)",
  follow: "var(--accent-text)", // "follow-press" shown as accent on the config surface
};

export function LedDot({ color, effect }: { color: LedColor; effect: LedEffect }) {
  const cls = `kc-led kc-led-${effect === "none" ? "steady" : effect}`;
  return <span className={cls} style={{ background: COLOR_VAR[color] }} aria-hidden />;
}
