/* Live key state for the Input view, driven by `board-event` frames.
 *
 * Firmware with raw edges (protocol "key" down/up for every key) lights a key
 * exactly while it is held: down lights it, up puts it out. Firmware without
 * them only reports the resolved gesture (single / double / long), which comes
 * on release or after the double-click window, so a gesture alone lights the
 * key briefly instead (PULSE_MS). The EC11 rotation arrives as signed detents;
 * the knob keeps an angle, the direction of the last turn and the running
 * count of the current burst. Pure functions: no timers, no React. */

export type KnobState = {
  /** Degrees; clockwise is positive. Grows without wrapping so a CSS transition never spins back. */
  angle: number;
  /** Direction of the last turn while it is recent, 0 when idle. */
  dir: -1 | 0 | 1;
  /** Detents turned in the current burst (signed), 0 when idle. */
  burst: number;
  /** Time (ms) of the last turn. */
  at: number;
};

export type LiveKeysState = {
  /** Keys held down right now, from raw edge frames. */
  down: Record<string, true>;
  /** Last time (ms) an edge frame was seen per key. */
  edgeAt: Record<string, number>;
  /** Gesture-only fallback: the key stays lit until this time (ms). */
  pulse: Record<string, number>;
  knob: KnobState;
};

export type BoardKeyEvent = {
  kind: string;
  id?: string;
  action?: string;
  detents?: number;
};

/** How long a gesture lights a key when no edge frames are available. */
export const PULSE_MS = 450;
/** Edge frames seen this recently mean the firmware sends them for that key. */
export const EDGE_FRESH_MS = 5000;
/** A turn within this window of the previous one belongs to the same burst. */
export const BURST_MS = 900;
/** Degrees per detent (an EC11 has about 24 detents per turn). */
export const DEG_PER_DETENT = 15;

const GESTURES = new Set(["single", "double", "long"]);

export const initialLiveKeys: LiveKeysState = {
  down: {},
  edgeAt: {},
  pulse: {},
  knob: { angle: 0, dir: 0, burst: 0, at: 0 },
};

export function onBoardEvent(s: LiveKeysState, ev: BoardKeyEvent, now: number): LiveKeysState {
  if (ev.kind === "encoder" && typeof ev.detents === "number" && ev.detents !== 0) {
    const sameBurst = now - s.knob.at <= BURST_MS && s.knob.burst !== 0;
    const burst = (sameBurst ? s.knob.burst : 0) + ev.detents;
    const angle = Math.max(-1e6, Math.min(1e6, s.knob.angle + ev.detents * DEG_PER_DETENT));
    return { ...s, knob: { angle, dir: ev.detents > 0 ? 1 : -1, burst, at: now } };
  }
  if (ev.kind !== "key" || !ev.id) return s;
  const id = ev.id;
  if (ev.action === "down") {
    return { ...s, down: { ...s.down, [id]: true }, edgeAt: { ...s.edgeAt, [id]: now } };
  }
  if (ev.action === "up") {
    const down = { ...s.down };
    delete down[id];
    return { ...s, down, edgeAt: { ...s.edgeAt, [id]: now } };
  }
  if (ev.action && GESTURES.has(ev.action)) {
    const fresh = now - (s.edgeAt[id] ?? -Infinity) <= EDGE_FRESH_MS;
    // With edge frames the key already lit on press and went out on release.
    return fresh ? s : { ...s, pulse: { ...s.pulse, [id]: now + PULSE_MS } };
  }
  return s;
}

/** Drop pulses and an idle knob burst once their time has passed. */
export function expire(s: LiveKeysState, now: number): LiveKeysState {
  let pulse = s.pulse;
  for (const id of Object.keys(s.pulse)) {
    if (s.pulse[id] <= now) {
      if (pulse === s.pulse) pulse = { ...s.pulse };
      delete pulse[id];
    }
  }
  const idle = s.knob.dir !== 0 && now - s.knob.at > BURST_MS;
  if (pulse === s.pulse && !idle) return s;
  return { ...s, pulse, knob: idle ? { ...s.knob, dir: 0, burst: 0 } : s.knob };
}

export function isLit(s: LiveKeysState, id: string, now: number): boolean {
  return s.down[id] === true || (s.pulse[id] ?? 0) > now;
}

/** Earliest time (ms) at which `expire` will change something, or null. */
export function nextExpiry(s: LiveKeysState): number | null {
  const times = Object.values(s.pulse);
  if (s.knob.dir !== 0) times.push(s.knob.at + BURST_MS + 1);
  return times.length > 0 ? Math.min(...times) : null;
}

/** Everything off, e.g. when the keyboard disconnects while a key is held. */
export function clearLiveKeys(s: LiveKeysState): LiveKeysState {
  return { ...initialLiveKeys, knob: { ...initialLiveKeys.knob, angle: s.knob.angle } };
}
