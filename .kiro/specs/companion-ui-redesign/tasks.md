# Companion UI Redesign — Tasks / 任务

> Pairs with `requirements.md` (R1–R8) and `design.md` (§1–§10). Check off as done.
> Each task lists its requirement refs and a concrete acceptance check.
> Verification note: Linux/EC2 can run `tsc` + `vite build` + `cargo check`; the FINAL visual/UX
> acceptance is the Tauri desktop app on Windows against a real AK — a browser preview is NOT proof.

## Phase 0 — Foundations / 基础

- [ ] **T0.1 Version auto-management (R7).** Add `companion/VERSION` (seed `0.1.0`) as single source;
  write `companion/tools/version.py` with `show/check/set/bump/auto-bump-staged` syncing the `version`
  field in `companion/src-tauri/tauri.conf.json`, `companion/src-tauri/Cargo.toml`,
  `companion/package.json`. Mirror firmware `tools/version.py` structure (validate semver,
  concurrent-edit guard).
  _Accept:_ `python3 companion/tools/version.py check` passes only when all 3 files == VERSION;
  `bump patch` updates all 3.
- [ ] **T0.2 pre-commit companion branch (R7.2/R7.3).** Extend `.githooks/pre-commit` to also run the
  companion `auto-bump-staged` when staged paths match `companion/src/**` or
  `companion/src-tauri/src/**`; idempotent if VERSION already bumped; refuse to auto-stage a version
  file carrying unrelated edits.
  _Accept:_ committing a companion source change bumps patch across 3 files in one commit;
  committing docs/config-only does not bump.

## Phase 1 — Theme tokens & providers / 令牌与主题 (R1, R6, R8)

- [ ] **T1.1 Token files.** Create `companion/src/theme/kc-dark.css` and `kc-light.css` defining the
  full semantic token block (surface/text/accent incl. `--accent-text`/semantic/border/radius/shadow/
  font), values calibrated to R8 (soft foregrounds, desaturated accent, no pure #fff/#000).
- [ ] **T1.2 ThemeProvider.** `companion/src/theme/index.ts` — `THEMES` map + `data-theme` on root +
  persistence to `~/.ak-companion/ui.json`. Components reference tokens only; none branch on theme.
- [ ] **T1.3 A11y/calm audit gate (R8, accent-text).** Verify both themes: body text ≈7:1 (not
  glaring), `--accent` never used as text on `--bg` (use `--accent-text`), steady-state status
  neutral, saturated color only for alerts.
  _Accept:_ swapping `data-theme` recolors the whole app with zero component edits (A1); no purple
  text on dark; light mode has no pure-white large surface.

## Phase 2 — i18n architecture / 国际化 (R2)

- [ ] **T2.1 i18n scaffold.** `companion/src/i18n/{en.json,zh.json,index.ts}`; `t(key, vars?)` with
  fallback locale→en→key; keys namespaced `conn.*/input.*/control.*/logs.*/common.*/help.*`.
- [ ] **T2.2 String extraction.** Replace every user-facing literal in the UI with `t(key)`; populate
  `en` and `zh` fully (no partial keys), including `help.*` tooltip bodies.
- [ ] **T2.3 Language switch.** Selector in Advanced settings; persisted to `ui.json`.
  _Accept:_ switching locale swaps all visible strings incl. tooltips; no literal leaks (A2).

## Phase 3 — App shell, header, modes / 外壳·头部·模式 (R3, brand)

- [ ] **T3.1 AppShell + providers.** Thin `main.tsx` to Theme/I18n/Mode providers + `<AppShell>`.
- [ ] **T3.2 Header.** BrandMark showing **"Companion for Kiro Crew"** tagline under the name;
  BoardStatusPill; ModeToggle [Simple|Advanced]; SettingsPopover (theme/language/skin); footer
  version from `VERSION` (R7.4).
- [ ] **T3.3 Mode system (R3).** `ModeProvider` + persisted mode; Tauri window-resize command
  (Simple ~420×620, Advanced ~960×720); one shared state store — no refetch on switch.
- [ ] **T3.4 SimpleView.** Connection state + Connect/Disconnect, voice state, "Watching: <chat>",
  link to advanced. No mechanism internals.
  _Accept:_ Simple↔Advanced resizes window and shows the specified control sets, preserving live
  connection/state (A3).

## Phase 4 — Advanced views reskin / 进阶视图改造 (R6)

- [ ] **T4.1 ConnectionView.** Transport (USB/BLE), port/BLE, Wi-Fi/License/QR, volume/brightness
  sync — reskinned into `Card`/`Field`/`StateBadge`; soft surfaces, no hard boxes.
- [ ] **T4.2 InputView (v0.3).** Live visualizer for the v0.3 inputs — K1-K6 + the EC11 rotary
  (rotate + push), NOT the retired v0.2 5-way joystick. Show each key's live edge and, for K1-K3,
  the classified click/double; control on/off segmented.
- [ ] **T4.3 LogsView.** Event stream Detailed/Raw, using `--mono`.

## Phase 4.5 — v0.3 interaction config surfaces / v0.3 交互配置面 (design §0; plan A keymap)

> These extend the crew-api-era baseline with the confirmed v0.3 interaction model. The firmware
> already emits click/double/cancel/confirm and accepts a `keymap` downlink frame (track A, shipped);
> this phase is the Companion UI that configures and visualizes it. The chosen MOCKUP is the spec —
> record which option was picked before building.

- [ ] **T4.5.1 Keymap config panel (plan A, stage 甲 — see design §5.5).** Default = **board-map**
  (option C): physical K1-K6 + EC11 grid, each cell showing its role + a live LED dot mirroring the
  board's real-time light; clicking a key opens an inspector with that role's **read-only
  (state×gesture)→(action,LED) rule rows** (data from `src/keymap/roleRules.ts`) plus the one
  editable control — the **role picker** (none/chat/voice/cancel/confirm). An **"Advanced — table
  view"** disclosure flips to option A: dense one-row-per-key table (role + K1-K3 chat#/instance#
  inline), all 7 keys at once. Both call `set_keymap` with the 7-slot role vector; the board acks;
  an empty role leaves that key unchanged. Behaviour is role-defined and NOT per-rule editable in 甲
  (that is stage 乙, deferred — see design §5.5.4). Advanced-only (Simple shows none).
  _Accept:_ board-map default renders 7 keys with live LED dots; clicking a key shows its role's
  rule rows + role picker; advanced disclosure shows the table; changing a role + Apply downlinks
  `set_keymap`; the roleRules table matches design §5.5.2.
- [ ] **T4.5.2 K1-K3 chat/instance binding.** For each CHAT key, two configurable numbers: the chat #
  it jumps to on single-click (Alt+N, existing `kc_bind_set`) and the Crew instance # it switches to
  on double-click (Ctrl+N — Local / Remote Crew; `BindStore.instances`). Show the resolved shortcut
  (e.g. "K1: click → chat 1 (Alt+1) · double → Local (Ctrl+1)"). Advanced-only.
  _Accept:_ both numbers persist and sync to the live router; the resolved-shortcut line matches.
- [ ] **T4.5.3 Pending-confirm + LED semantics (read-only visualizer).** When the board is in
  post-ASR pending-confirm, surface it: show the ASR text awaiting confirmation and the active
  edit affordances (K4 click = delete char / long = delete all; K6 click = confirm). Render the LED
  semantics legend (color = state: green idle / red busy / orange waiting / blue info; effect =
  gesture: single blink / double blink / breathing) so a user can read the board's lights. Present
  in both modes (Simple = compact "Confirming: <text>"; Advanced = full legend). Driven by the
  board's `voice`/state frames — no new protocol.
  _Accept:_ a pending-confirm state on the board shows the awaiting text + the K4/K6 affordances;
  the LED legend matches design §0.3/§0.5.

## Phase 5 — Control: API+WS primary + legacy collapsed / 控制重构 (R4)

- [ ] **T5.1 Mechanism selector.** Persisted `[ API+WebSocket | Legacy shortcuts ]`; API expanded by
  default, legacy collapsed.
- [ ] **T5.2 API panel — endpoint & auth.** Endpoint (127.0.0.1:5476 / KIROCREW_PORT) + Test probe;
  auth selector (internal secret / app token) with AUTH OK / 403 / secret-not-found status.
- [ ] **T5.3 API panel — WS live link + chat list.** WS `/api/ws` state; chat list from
  `GET /api/chat/slots` (key/title/state); one-click **Watch** pins the mirrored slot.
- [ ] **T5.4 API panel — send test.** `POST /api/chat {slot,message}` to verify control direction.
- [ ] **T5.5 Legacy chain (collapsed).** Preserve the 4 steps verbatim (bind window / map chats /
  install hook + listener / link session IDs) with gating + `(i)` help under a disclosure.
  _Accept:_ mechanism switch toggles the two paths and persists; API path drives Watch via stable
  slot key with no binding dance; legacy still functions (A4).

## Phase 6 — Tooltip help pass / 浮层说明 (R5)

- [ ] **T6.1 InfoTip component.** `<InfoTip k="help.*" />` → `(i)` with hover/focus popover, keyboard
  accessible (role, aria-describedby, Esc closes), `--panel`/`--shadow-md`/`--radius-md`.
- [ ] **T6.2 Strip inline hints.** Move every `hint` paragraph into `help.*` popovers; default surface
  shows no explanatory prose.
  _Accept:_ no explanatory paragraph visible by default; all behind `(i)` (A5).

## Phase 7 — Persistence & final verify / 持久化与验收

- [ ] **T7.1 ui.json persistence.** `{theme, locale, mode}` read on startup, written on change; Rust
  read/write command + safe defaults.
- [ ] **T7.2 Skin extensibility proof (R1.4).** Add one extra skin as a token-only file; confirm it
  renders fully with zero `.tsx` change.
- [ ] **T7.3 Build gates.** `tsc` + `vite build` + `cargo check --lib` green; `version.py check`
  green; `git diff --check` clean.
- [ ] **T7.4 On-hardware acceptance (A6).** Run the Tauri desktop app on Windows against a real AK;
  verify theme/lang/mode switching, Control API+WS path end-to-end, legacy fallback, and that no
  surface is glaring in either light or dark (R8). Browser/Vite preview does NOT count.

## Out of scope / 不在本 spec
Firmware changes; protocol frame changes; migrating old `~/.ak-companion` data; backend command
surface beyond window-resize + `ui.json` I/O + the crew-api client (that client is the separate
`feat/companion-crew-api` workstream).

## Suggested commit slices / 建议提交切片
1. T0 (version mgmt) · 2. T1 (tokens+provider) · 3. T2 (i18n) · 4. T3 (shell/header/modes) ·
5. T4 (advanced reskin) · 6. T5 (control) · 7. T6 (tooltips) · 8. T7 (persist+verify).
Each slice keeps `tsc`/`vite`/`cargo check` green so the branch is always buildable.
