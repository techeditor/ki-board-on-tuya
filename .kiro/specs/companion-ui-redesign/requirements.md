# Companion UI Redesign — Requirements / 需求

> Status: DRAFT (design only, no implementation)
> Branch context: developed alongside `feat/companion-crew-api`
> Baseline reviewed: `companion/src/main.tsx` (1821 lines) + `companion/src/styles.css` on this branch.

## 1. Motivation / 动机

The current Companion UI (neo-brutalist: 2px hard borders + offset drop-shadows + monospace,
everything boxed) wastes space, over-uses frames, and has weak visual hierarchy. Because Companion
exists *to drive Kiro Crew*, its look should follow Kiro Crew's own dashboard language rather than a
separate brutalist identity.

当前 Companion 界面（neo-brutalist：2px 硬边框 + 位移阴影 + 等宽字体、满屏方框）空间浪费、方框过多、
视觉层次不清。Companion 是为驱动 Kiro Crew 而生，其视觉应贴近 Kiro Crew 仪表盘语言，而非另起一套。

## 2. Goals / 目标

- **G1 — Kiro-Crew-aligned visual language.** Reuse Kiro Crew's semantic CSS-variable token system
  verbatim (same variable NAMES), so skins/dark-light come for free. 复用 Kiro Crew 语义变量令牌体系。
- **G2 — Themable & localizable by construction.** Language (zh/en), dark/light, and swappable skins
  are all first-class extension points left open from day one. 语言/明暗/皮肤均为一等扩展点。
- **G3 — Simple vs Advanced modes.** A global toggle: Simple = small window with only essential
  controls (status, connect, watched chat, voice); Advanced = large window with the full config
  surface. 简约（小窗）/进阶（大窗）全局切换。
- **G4 — Coherent LOCAL CTRL.** Present the shortcut-control causal chain as ONE guided flow, not
  four parallel boxes. 把快捷键控制的因果链做成一条引导式流程。
- **G5 — Tooltip-only help.** All long explanations move into `(i)` hover popovers; the surface shows
  only controls + state. 所有详细说明收进 `(i)` 悬浮浮层，界面只留必要信息。

Non-goals: firmware changes; protocol changes; migrating existing user data; touching the Rust
backend command surface beyond what the UI needs (backend is out of scope for THIS spec).

## 3. Design tokens (reused from Kiro Crew) / 复用的设计令牌

Confirmed present in Kiro Crew's compiled theme CSS — the redesign MUST use the same names:

| Group | Tokens |
|---|---|
| Surface | `--bg`, `--surface`, `--panel`, `--field` |
| Text | `--text`, `--muted`, `--subtle` |
| Accent | `--accent`, `--accent-hover`, `--accent-text`, `--accent-subtle`, `--accent-glow`, `--accent-fg` |
| A11y note | `--accent` is a FILL color (button/badge bg with `--accent-fg` on top); NEVER a text color on `--bg`. Purple text on a dark canvas fails contrast — use `--accent-text` (a lighter accent tuned to ≥4.5:1 on `--bg`) for any accent-colored foreground. |
| Semantic | `--success`, `--warning`, `--danger`, `--blue` |
| Border | `--border`, `--border-strong` |
| Radius | `--radius-sm:6px`, `--radius-md:8px`, `--radius-lg:12px`, `--radius-xl:16px` |
| Shadow | `--shadow-sm`, `--shadow-md`, `--shadow-lg` |
| Font | `--font-body` (Space Grotesk stack), `--mono` (JetBrains Mono stack) |

Kiro Crew ships MANY themes by assigning different VALUES to these same names, switched via a
`data-theme` attribute on the root. The Companion redesign adopts the identical mechanism: **skin =
swap token values, never touch components.** 皮肤切换=换令牌值，不动组件。

## 4. Functional requirements / 功能需求

### R1 Theme & skin system
- R1.1 Root carries `data-theme` (e.g. `kc-dark`, `kc-light`); all colors reference tokens only.
- R1.2 Ship at least one dark + one light theme at parity with Kiro Crew's palette.
- R1.3 A theme picker lives in Advanced settings; the chosen theme persists to `~/.ak-companion/`.
- R1.4 Adding a skin = adding one token block; no component edits (acceptance: a new skin file with
  no `.tsx` change renders fully).

### R2 Internationalization (architecture first)
- R2.1 All user-facing strings resolve through a `t(key)` lookup, never inline literals.
- R2.2 Locale files `en.json` / `zh.json`; language switch in Advanced settings, persisted.
- R2.3 THIS phase: extract keys + provide zh + en strings side by side; no partial/omitted keys.
- R2.4 Fallback chain: selected locale → en → key name.

### R3 Simple / Advanced modes
- R3.1 Global toggle in a persistent header control (not buried in a view).
- R3.2 Simple mode window ~ 420×620; shows only: connection state, connect/disconnect, transport
  badge, watched-chat one-liner, voice state, mode toggle, settings entry.
- R3.3 Advanced mode window ~ 960×720; shows the full navigation + all config surfaces.
- R3.4 Both modes read ONE shared state store — switching never loses connection or re-fetches.
- R3.5 Mode choice persists across launches.

### R4 Control — API+WS primary, legacy chain collapsed (fallback)
The Control view offers two mechanisms via a persisted selector; the NEW API+WebSocket path is
primary and expanded by default, the LEGACY shortcut chain is collapsed as fallback.
- R4.1 **API+WS (primary).** Endpoint (127.0.0.1:5476) + Test; auth selector (internal secret / app
  token) with status; WS live-link state; chat list pulled from `GET /api/chat/slots` (stable slot
  keys); one-click **Watch** pins the board's mirrored slot; a send-test injects via `POST /api/chat`.
- R4.2 **Legacy shortcuts (fallback, collapsed).** The prior four-step chain — bind window, map chats
  to Alt+N, install hook + start listener, link session IDs to bound chats — preserved verbatim under
  a disclosure, for non-co-located machines or when the API is unavailable.
- R4.3 A mechanism selector at the top of the view switches between them; choice persists.
- R4.4 Each legacy step keeps its done/undone gating; API path shows AUTH/LINK/WATCH state inline.
- R4.5 Simple mode shows only the resulting "Watching: <chat>" + link state, never mechanism internals.

### R7 Version auto-management / 版本自动管理
- R7.1 Single source of truth `companion/VERSION` (semver); `companion/tools/version.py` syncs it into
  tauri.conf.json, Cargo.toml, and package.json (`show/check/set/bump/auto-bump-staged`, mirroring
  firmware's `tools/version.py`).
- R7.2 `.githooks/pre-commit` auto-bumps patch + stages synced files when staged changes touch
  companion implementation (not config/docs only); idempotent if VERSION already bumped.
- R7.3 Concurrent-edit guard refuses to auto-stage a version file carrying unrelated edits.
- R7.4 The header shows the resolved version from `VERSION`, not a hard-coded literal.

### R8 Calm color / 柔和配色（不刺眼）
Comfort over maximum contrast. Body/surface colors sit in a soft, low-strain range; saturated,
high-contrast color is reserved for genuine attention signals only. 舒适优先于最大对比。
- R8.1 No pure `#fff` text on dark, no pure `#000`/near-black text on light — use softened
  foregrounds (dark: `~#e6e1f0`; light: `~#352d47`), comfortable ≈7:1, not glaring extremes.
- R8.2 No pure-white (`#ffffff`) large surfaces in light mode; use a tinted soft white (`~#faf8fe`).
- R8.3 Accent is DESATURATED for large/persistent fills (buttons, active states); full-saturation
  accent is not used across big areas. 大面积/常驻填充用降饱和强调色。
- R8.4 Steady-state status (RUNNING/IDLE, connected) uses NEUTRAL or lightly-tinted styling, not a
  vivid color — vivid `--success/--warning/--danger` is only for a real transition/alert
  (error, warning, the moment something just succeeded). 常态状态用中性；鲜亮色只留给真正的提示/告警。
- R8.5 The one exception this rule protects: special alerts MAY be high-contrast on purpose. Every
  other surface must read calm.
- R8.6 Both light and dark themes are audited against R8.1–R8.5; new skins must pass the same bar.

### R5 Tooltip-only help
- R5.1 Every control that today carries a `hint` paragraph instead gets an `(i)` affordance.
- R5.2 Help text appears on hover/focus of `(i)` as a floating popover; keyboard-accessible.
- R5.3 The default surface shows NO explanatory paragraphs — only labels, controls, state.
- R5.4 Popover content flows through `t(key)` (localized like everything else).

### R6 Visual hierarchy (fixing "too many boxes")
- R6.1 Replace hard 2px borders + offset shadows with Kiro-Crew-style soft surfaces: hierarchy from
  spacing + subtle `--surface`/`--panel` layering + `--radius-*`, not from outlining every element.
- R6.2 One accent color + glow for primary/active state; neutral elsewhere.
- R6.3 Generous whitespace; group related controls without wrapping each in its own frame.

## 5. Acceptance / 验收
- A1 Switching `data-theme` recolors the entire app with zero component changes.
- A2 Switching locale swaps all visible strings incl. tooltips; no literal leaks.
- A3 Simple↔Advanced toggle resizes the window and shows the specified control sets, preserving live
  connection/state.
- A4 Control shows a persisted mechanism selector; API+WS path is default-expanded (endpoint/auth/WS
  status + chat list from `/api/chat/slots` + one-click Watch), legacy 4-step chain is collapsed and
  still functional; Simple mode shows only "Watching: <chat>".
- A5 No explanatory paragraph is visible by default; all live behind `(i)` popovers.
- A6 Final verification is running the Tauri desktop app against a real AK — NOT a browser preview.
