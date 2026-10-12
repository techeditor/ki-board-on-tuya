# Companion UI Redesign — Design / 设计

> Design only. Pairs with `requirements.md` in this spec folder.

## 1. Information architecture / 信息架构

```
App shell
├─ Header (persistent, both modes)
│   ├─ Brand: Agentic Keyboard — "Companion for Kiro Crew" (tagline under name)
│   ├─ Board status pill  (ONLINE/OFFLINE · transport)
│   ├─ Mode toggle        [ Simple | Advanced ]   ← global, resizes window
│   └─ Settings (⚙)       → theme picker, language, skin  (popover)
│
├─ SIMPLE MODE  (small window ~420×620, single column)
│   ├─ Connection card    (state + Connect/Disconnect)
│   ├─ Voice state        (LISTENING/IDLE)
│   ├─ Watched chat       ("Watching: <name>"  or  "Following all")
│   └─ link → "Open advanced settings"
│
└─ ADVANCED MODE (large window ~960×720, left nav + content)
    ├─ Nav: Connection · Input · Control · Logs
    ├─ Connection   (transport, port/BLE, Wi-Fi, License, QR, volume/brightness sync)
    ├─ Input        (live K1–K5 + 5-way visualizer, control on/off)
    ├─ Control      (API+WS primary; legacy shortcut chain collapsed, see §4)
    └─ Logs         (event stream, Detailed/Raw)
```

Naming is unified: the third area is **Control** everywhere (nav label, title, internal steps) —
retiring the current `kirocrew` / `LOCAL CTRL` / "Bind the Window" three-way naming split.
第三区统一命名为 **Control**。

## 2. Token mapping / 令牌映射

The redesign consumes Kiro Crew's tokens directly. Old brutalist styling → new mapping:

| Old (styles.css) | New (Kiro-Crew token) | Note |
|---|---|---|
| `border: 2px solid` everywhere | `1px solid var(--border)` on containers ONLY | stop outlining every element |
| offset `box-shadow: 2px 2px 0` | `var(--shadow-sm/md)` | soft depth, not hard offset |
| `border-radius: 0` | `var(--radius-md/lg)` | soft corners |
| `"Courier New"` mono everywhere | `var(--font-body)` for UI, `var(--mono)` for IDs/logs | body = Space Grotesk |
| `--accent:#8b5cf6` | `var(--accent)` (theme-provided; KC purple `#8e48ff` family) | single accent + glow |
| accent AS TEXT on dark | `var(--accent-text)` (lighter, ≥4.5:1 on `--bg`) | `--accent` is fill-only; purple text on dark fails contrast |
| ad-hoc `--panel/--field` | `var(--surface)/var(--panel)/var(--field)` | layered surfaces |

Hierarchy comes from **surface layering + spacing + one accent**, never from bordering everything.
层次来自表面分层+间距+单一强调色，而非给每个元素描边。

## 3. Theme & i18n architecture / 主题与国际化架构

### 3.1 Theme
- Root element gets `data-theme="kc-dark" | "kc-light" | <skin-id>`.
- `themes/kc-dark.css`, `themes/kc-light.css` each define the full token block (values only).
- A skin is a drop-in token file registered in a small `THEMES` map; components never branch on theme.
- Persisted to `~/.ak-companion/ui.json` → `{ theme, locale, mode }`.

### 3.2 i18n
- `src/i18n/en.json`, `src/i18n/zh.json`: flat key→string maps.
- `t(key, vars?)` helper with fallback locale→en→key.
- Keys namespaced by area: `conn.*`, `input.*`, `control.*`, `logs.*`, `common.*`, `help.*`.
- Tooltip bodies live under `help.*` and resolve through the same `t()`.
- This phase ships both `en` and `zh` fully populated (no partial keys).

## 4. Control — API+WS primary, legacy shortcut chain collapsed / 控制

Control offers TWO mechanisms with a switch at the top of the view. The NEW API+WebSocket path is
primary and shown by default; the LEGACY shortcut-injection chain is collapsed under a disclosure and
kept as a fallback. 控制提供两套机制，顶部切换：新的 API+WS 为主、默认展开；旧的快捷键注入链折叠保留作兜底。

```
Control view
├─ Mechanism switch   ( ● API + WebSocket   ○ Legacy shortcuts )   ← persisted
│
├─ [PRIMARY] API + WebSocket                              (shown by default)
│   ├─ Endpoint    127.0.0.1:5476  (KIROCREW_PORT)        [ Test ]
│   ├─ Auth        ( ● Internal secret  ○ App token )     (i)
│   │              status: AUTH OK / 403 / secret not found
│   ├─ Live link   WS /api/ws        state: CONNECTED · heartbeat 30s
│   ├─ Chats       (pulled from GET /api/chat/slots — real slot keys, stable across restart)
│   │      ┌ slot key            title            state      ┐
│   │      │ chat-4-1726…   Refactor auth    RUNNING   [Watch] │  ← Watch = board follows this slot
│   │      │ chat-5-1726…   Firmware LCD     IDLE      [Watch] │
│   │      └───────────────────────────────────────────────────┘
│   └─ Send test   [ text ____________ ] → POST /api/chat {slot,message}
│
└─ [LEGACY] Shortcut injection (fallback)                 ▸ collapsed disclosure
    ├─ Step 1  Bind Kiro Crew window          (title match + SendInput)
    ├─ Step 2  Map chats to shortcuts         (Chat A/B/C → Alt+N)
    ├─ Step 3  Install hook + start listener  (127.0.0.1:5477, agent status)
    └─ Step 4  Link session IDs to bound chats (filter watched chat)
```

### 4.1 Why API+WS is primary / 为什么 API+WS 为主
The gateway exposes a local REST+WebSocket API (127.0.0.1:5476, loopback-only). Compared to the
legacy chain it removes every fragile piece: no window focus race, no Alt+N position vs session_id
mismatch, no SendInput scancodes, no separate hook process. Slot keys are STABLE across restart, so
"which chat" is unambiguous. Control = `POST /api/chat {slot,message}`; live status = `GET /api/ws`.
This is the mechanism the `feat/companion-crew-api` branch is building.

### 4.2 API path UI / 新方式界面
- **Endpoint + Test**: shows the resolved port; Test probes reachability + auth in one click.
- **Auth selector**: Internal secret (re-read each connect, all-access loopback) vs App token
  (scoped, ~1h). Status line reports AUTH OK / 403 / secret-not-found. Detail behind `(i)`.
- **Live link**: WS connection state; drives the chat list and the board LCD in real time.
- **Chat list**: rows come straight from `/api/chat/slots` (key/title/state/pinned). One-click
  **Watch** pins the slot the board mirrors — no Step-2/Step-4 binding dance needed, the slot key IS
  the identity.
- **Send test**: inject text to a slot to verify the control direction.

### 4.3 Legacy path (collapsed) / 旧方式（折叠）
The four-step chain from the previous design is preserved verbatim under a collapsed disclosure, for
machines where companion and gateway are NOT co-located (API needs loopback) or the API is
unavailable. Switching the mechanism selector to "Legacy shortcuts" expands it and routes control
through SendInput again. Its own steps keep their gating and `(i)` help.

### 4.4 Mode interaction / 与简约模式的关系
- Simple mode shows only the resulting "Watching: <chat>" plus link state — never the mechanism
  internals, regardless of which mechanism is active.
- The mechanism selector, endpoint/auth, chat list, and legacy chain are all Advanced-only.

## 5. Simple vs Advanced / 简约与进阶

| | Simple | Advanced |
|---|---|---|
| Window | ~420×620 | ~960×720 |
| Layout | single column, no nav | left nav + content |
| Shows | status, connect, voice, watched | everything |
| Config | none (link to advanced) | transport/Wi-Fi/License/QR/binds/hook/logs |
| Shared | one state store, one connection, persisted mode | same |

Switching calls the Tauri window-resize API and flips a `mode` flag; no data refetch.

## 5.5 Keymap config — per-gesture action binding (stage 乙, simplified) / 按键配置

The v0.3 keymap panel (Phase 4.5) lets the user **bind an action to each gesture
of each key, one gesture per row**. This supersedes stage 甲 (role-only, read-only
rules). The simplification that makes it tractable: **only the voice key needs the
immediate-edge path; every other key is an ordinary key** that runs the standard
single/double/long gesture classifier. So the UI never exposes the word "edge" —
there is just the voice key (special, hold-to-talk, no gesture rows) and ordinary
keys (click/double/long rows, each freely bound to an action).

### 5.5.1 Per-key model
Each of K1-K6 + EC11 is one of two KINDS:
- **`voice`** — hold-to-talk. Internally an immediate-edge PTT key (down starts
  recording, up stops). It has NO gesture rows; the UI shows it as a single
  "Voice" tile. Default: K5.
- **`normal`** — the standard key. Runs the single/double/long classifier and
  carries three gesture SLOTS: `click`, `double`, `long`, each bound to one
  ACTION (or unbound). Default on the others.

`long` fires immediately when the hold threshold (~800ms) is crossed (desktop
long-press feel); that press's `click`/`double` then does not also fire.

### 5.5.2 Action library (what a gesture slot can bind)
| action | wire | meaning | default LED (color · effect) |
|---|---|---|---|
| `none` | `none` | unbound | — |
| `chat:N` | `chat1`…`chat9` | jump to chat N (Alt+N) | follow-press · single |
| `instance:N` | `instance1`…`instance9` | switch Crew instance N (Ctrl+N) | follow-press · double |
| `cancel_char` | `cancel_char` | delete one char (pending-confirm) | red · single |
| `cancel_all` | `cancel_all` | delete all + exit pending | red · breathing |
| `confirm` | `confirm` | confirm / submit + exit pending | green · single |

The action library is extensible (command bar, new chat, macros) without a model
change — add a row here. LED color/effect is DERIVED from the action for now (a
default map); per-row custom LED is a later increment (乙-全), out of scope.

### 5.5.3 Two entry points (unchanged from the board-map decision)
- **Board map (default, option C).** Physical K1-K6 + EC11 grid, live LED dot per
  key mirroring the board. Click a key → inspector showing its KIND and, for a
  normal key, the three gesture rows (click/double/long), each with an action
  picker. The voice key's inspector just says hold-to-talk.
- **Table (advanced, option A).** Dense editor: one block per key, its gesture
  rows inline. Same data, same downlink.

### 5.5.4 Protocol / firmware contract (the big change vs 甲)
The firmware keymap moves from "one role per key" to "a per-gesture action map
per key", and the `keymap` downlink frame carries the bindings. To stay within
the flat `ak_json_str` extractor (no cJSON), the wire uses flat dotted keys —
one field per (key, gesture):

```
{"v":1,"type":"keymap","payload":{
  "k1.click":"chat1","k1.double":"instance1","k1.long":"none",
  "k4.click":"cancel_char","k4.long":"cancel_all",
  "k5.kind":"voice",
  "k6.click":"confirm", ...}}
```

- `<key>.kind` = `voice` makes that key the PTT key (no gesture fields read).
- `<key>.click` / `.double` / `.long` = an action wire name from §5.5.2.
- Omitted fields keep the board's current binding (partial update).
- Firmware stores the resolved table in TAL KV (versioned blob, as before).
- The companion route maps each delivered gesture action to its injection
  (chatN→Alt+N, instanceN→Ctrl+N, cancel_*/confirm→the composer edits).

This replaces the 7-role `set_keymap` of 甲. `src/keymap/roleRules.ts` is retired
in favour of `src/keymap/actions.ts` (the action library + default-LED map) and a
`KeymapState` of per-key gesture bindings.

### 5.5.5 LED legend + live mirror (unchanged)
The board map renders the §0.3/§0.5 LED legend and each key dot reflects the
board's current light, doubling as the LED status view.

## 6. Tooltip component / 浮层组件

- `<InfoTip k="help.control.step1" />` renders a small `(i)`; on hover/focus shows a popover with
  `t(k)`.
- Popover: `var(--panel)` bg, `--shadow-md`, `--radius-md`, max-width ~280px, arrow to trigger.
- Accessible: `role="button"`, `aria-describedby`, opens on focus, Esc closes, dismiss on blur.
- No explanatory `<p class="hint">` remains in the default surface (req R5.3).

## 7. Component inventory / 组件清单 (new)

`ThemeProvider`, `I18nProvider`, `ModeProvider` (context) · `AppShell` · `Header` (BrandMark,
BoardStatusPill, ModeToggle, SettingsPopover) · `SimpleView` · `AdvancedView` (NavRail, view router)
· `ConnectionView` · `InputView` · `ControlView` (StepCard ×4, SessionList) · `LogsView` · shared
`InfoTip`, `StateBadge`, `Field`, `Card`.

## 8. File plan (proposed, not created) / 文件规划

```
companion/src/
  main.tsx                  (thinned: providers + shell only)
  theme/tokens.css          (@import the active theme)
  theme/kc-dark.css
  theme/kc-light.css
  theme/index.ts            (THEMES map + persistence)
  i18n/en.json  zh.json  index.ts (t helper)
  ui/  AppShell, Header, InfoTip, Card, StateBadge, Field …
  views/ SimpleView, ConnectionView, InputView, ControlView, LogsView
```

Backend (`lib.rs`, `status_sink.rs`, `crew_api.rs`) is unchanged by this UI spec except for a
window-resize command for mode switching and a `ui.json` read/write for persistence.

## 9. Migration order / 迁移顺序 (for tasks.md)
1. Token + ThemeProvider (dark/light) — visual reskin only, keep old IA.
2. i18n extraction (en+zh) behind `t()`.
3. AppShell + Header + Mode toggle + Simple view.
4. Advanced Connection/Input/Logs reskinned into new components.
5. Control: API+WS primary panel + collapsed legacy chain (the biggest behavioral regroup).
6. InfoTip pass — strip all inline hints into `help.*` popovers.
7. Persist `ui.json`; final on-hardware verification.

## 10. Companion version auto-management / 版本自动管理

Mirrors the firmware scheme (`firmware/app/VERSION` + `tools/version.py auto-bump-staged` +
`.githooks/pre-commit`). Today the companion has THREE hand-written, drifting version strings:
`companion/src-tauri/tauri.conf.json`, `companion/src-tauri/Cargo.toml`, `companion/package.json`
(all `0.1.0`). The redesign introduces single-source auto-management from day one.

- V1 **Single source of truth**: `companion/VERSION` (semver `MAJOR.MINOR.PATCH`).
- V2 **Sync script** `companion/tools/version.py` with the same verbs as firmware:
  `show` / `check` (all three files agree) / `set X.Y.Z` / `bump {patch|minor|major}` /
  `auto-bump-staged`. It rewrites the `version` field in tauri.conf.json, Cargo.toml, package.json
  from `VERSION`.
- V3 **pre-commit integration**: the existing `.githooks/pre-commit` gains a companion branch —
  when staged changes touch `companion/src/**` or `companion/src-tauri/src/**` (implementation, not
  just config), auto-bump patch and stage the synced version files. Idempotent: if VERSION was
  already hand-bumped in the same staged set, do not bump again (same guard as firmware).
- V4 **Concurrent-edit guard**: like firmware's `ensure_managed_files_are_version_only`, refuse to
  auto-stage a version file that carries unrelated concurrent edits, so a parallel session's work is
  never swept into the version commit.
- V5 **Display**: the resolved version renders in the header footer (`v0.1.0`) via `VERSION`, not a
  hard-coded literal — Simple and Advanced both show it.

Acceptance: `companion/tools/version.py check` passes only when all three files equal `VERSION`;
committing a companion source change bumps patch across all three in one commit; committing docs-only
or config-only changes does not bump.
