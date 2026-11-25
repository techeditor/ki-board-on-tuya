/* Live view of the keyboard's six keys and the EC11 knob (Input view). A key
 * lights the moment the keyboard reports it pressed and goes out on release;
 * the knob is a round dial that turns with the rotation and shows the
 * direction and count of the current burst. State comes from app/liveKeys.ts. */
import { isLit, type LiveKeysState } from "../app/liveKeys";
import { useI18n } from "../i18n";

const KEYS = ["k1", "k2", "k3", "k4", "k5", "k6"] as const;

export function LiveKeys({ live }: { live: LiveKeysState }) {
  const { t } = useI18n();
  const now = Date.now();
  return (
    <div className="kc-live">
      <div className="kc-keys">
        {KEYS.map((id) => {
          const down = isLit(live, id, now);
          return (
            <div
              key={id}
              className={`kc-key ${down ? "down" : ""}`}
              role="img"
              aria-label={`${id.toUpperCase()} ${t(down ? "input.live.pressed" : "input.live.released")}`}
            >
              {id.toUpperCase()}
            </div>
          );
        })}
      </div>
      <Knob live={live} down={isLit(live, "enc", now)} />
    </div>
  );
}

function Knob({ live, down }: { live: LiveKeysState; down: boolean }) {
  const { t } = useI18n();
  const { angle, dir, burst } = live.knob;
  const turn = dir > 0 ? "cw" : dir < 0 ? "ccw" : "";
  return (
    <div className={`kc-knob ${down ? "down" : ""} ${turn}`}>
      <svg viewBox="0 0 100 100" role="img" aria-label={`EC11 ${t(down ? "input.live.pressed" : "input.live.released")}`}>
        <defs>
          <marker id="kc-head-off" viewBox="0 0 6 6" refX="3" refY="3" markerWidth="4" markerHeight="4" orient="auto">
            <path className="kc-knob-head off" d="M0 0 L6 3 L0 6 z" />
          </marker>
          <marker id="kc-head-on" viewBox="0 0 6 6" refX="3" refY="3" markerWidth="4" markerHeight="4" orient="auto">
            <path className="kc-knob-head on" d="M0 0 L6 3 L0 6 z" />
          </marker>
        </defs>
        <path
          className={`kc-knob-arc ccw ${dir < 0 ? "on" : ""}`}
          d="M42 4.7 A46 46 0 0 0 4.7 42"
          markerEnd={`url(#kc-head-${dir < 0 ? "on" : "off"})`}
        />
        <path
          className={`kc-knob-arc cw ${dir > 0 ? "on" : ""}`}
          d="M58 4.7 A46 46 0 0 1 95.3 42"
          markerEnd={`url(#kc-head-${dir > 0 ? "on" : "off"})`}
        />
        <g className="kc-knob-dial" style={{ transform: `rotate(${angle}deg)`, transformOrigin: "50px 50px" }}>
          <circle className="kc-knob-cap" cx="50" cy="50" r="31" />
          <line className="kc-knob-mark" x1="50" y1="24" x2="50" y2="36" />
        </g>
        <text className="kc-knob-id" x="50" y="54" textAnchor="middle">EC11</text>
      </svg>
      <div className="kc-knob-burst" aria-live="polite">
        {dir > 0 && `${t("input.live.cw")} +${burst}`}
        {dir < 0 && `${t("input.live.ccw")} ${burst}`}
      </div>
    </div>
  );
}
