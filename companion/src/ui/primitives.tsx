/* Shared UI primitives (R6 soft surfaces, R5 tooltip-only help). All colors come
 * from theme tokens — no literals, so they recolor with the theme. */
import { useId, useState, type ReactNode } from "react";

import { useI18n } from "../i18n";

/** A soft surface container — hierarchy from layering + spacing, not borders. */
export function Card({ title, info, children }: { title?: string; info?: string; children: ReactNode }) {
  return (
    <section className="kc-card">
      {title && (
        <h3 className="kc-card-title">
          {title}
          {info && <InfoTip k={info} />}
        </h3>
      )}
      {children}
    </section>
  );
}

/** A small status pill. `tone` picks a semantic token; steady states stay
 * neutral (R8.4) — pass "neutral" unless this is a real alert. */
export function StateBadge({
  tone = "neutral",
  children,
}: {
  tone?: "neutral" | "success" | "warning" | "danger" | "accent" | "blue";
  children: ReactNode;
}) {
  return <span className={`kc-badge kc-badge-${tone}`}>{children}</span>;
}

/** (i) affordance with a hover/focus popover carrying t(k) (R5). Keyboard
 * accessible: focusable, Esc closes, dismiss on blur. */
export function InfoTip({ k }: { k: string }) {
  const { t } = useI18n();
  const [open, setOpen] = useState(false);
  const id = useId();
  return (
    <span className="kc-infotip-wrap">
      <button
        type="button"
        className="kc-infotip"
        aria-describedby={open ? id : undefined}
        aria-label="More info"
        onMouseEnter={() => setOpen(true)}
        onMouseLeave={() => setOpen(false)}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onKeyDown={(e) => {
          if (e.key === "Escape") setOpen(false);
        }}
      >
        i
      </button>
      {open && (
        <span role="tooltip" id={id} className="kc-infotip-pop">
          {t(k)}
        </span>
      )}
    </span>
  );
}
