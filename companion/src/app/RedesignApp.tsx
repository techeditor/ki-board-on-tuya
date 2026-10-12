/* Redesign shell (R3/R6). Assembles the theme + i18n providers, the persistent
 * header (brand, status pill, Simple/Advanced toggle, theme + language), and the
 * content. This stage ships the v0.3 keymap panel + a pending-confirm / LED
 * visualizer; the Connection/Control/Logs views are reskinned in a later batch
 * and plug into the same nav.
 *
 * The set_keymap downlink goes through the Tauri invoke bridge; in a plain Vite
 * preview (no Tauri runtime) it logs instead, so the UI is reviewable in a
 * browser without a board. On-hardware verification runs the Tauri app (A6). */
import { useEffect, useMemo, useState } from "react";

import { ThemeProvider, THEMES, useTheme, type ThemeId } from "../theme";
import { I18nProvider, LOCALES, isLocale, useI18n, type Locale } from "../i18n";
import { KeymapPanel, defaultKeymapState, type KeymapState } from "../ui/KeymapPanel";
import { Card, StateBadge } from "../ui/primitives";
import { LedDot } from "../ui/LedDot";
import type { Role } from "../keymap/roleRules";

import "../theme/components.css";

type Mode = "simple" | "advanced";

/** Persisted UI prefs (R7/§8). Written to ~/.ak-companion/ui.json by the Rust
 * side; here we also mirror to localStorage so a browser preview persists. */
interface UiPrefs {
  theme: ThemeId;
  locale: Locale;
  mode: Mode;
}

const PREFS_KEY = "ak-ui-prefs";

function loadPrefs(): UiPrefs {
  try {
    const raw = localStorage.getItem(PREFS_KEY);
    if (raw) {
      const p = JSON.parse(raw) as Partial<UiPrefs>;
      return {
        theme: THEMES.some((t) => t.id === p.theme) ? (p.theme as ThemeId) : "kc-dark",
        locale: isLocale(p.locale) ? p.locale : "zh",
        mode: p.mode === "advanced" ? "advanced" : "simple",
      };
    }
  } catch {
    /* ignore malformed prefs */
  }
  return { theme: "kc-dark", locale: "zh", mode: "simple" };
}

function savePrefs(p: UiPrefs): void {
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(p));
  } catch {
    /* ignore */
  }
}

/** Call a Tauri command when the runtime is present; otherwise log (preview). */
async function invokeCmd(cmd: string, args: Record<string, unknown>): Promise<void> {
  const tauri = (window as unknown as { __TAURI__?: { core?: { invoke?: (c: string, a: unknown) => Promise<unknown> } } }).__TAURI__;
  const invoke = tauri?.core?.invoke;
  if (invoke) {
    await invoke(cmd, args);
  } else {
    // eslint-disable-next-line no-console
    console.info(`[preview] ${cmd}`, args);
  }
}

export function RedesignApp() {
  const prefs = useMemo(loadPrefs, []);
  const [theme, setTheme] = useState<ThemeId>(prefs.theme);
  const [locale, setLocale] = useState<Locale>(prefs.locale);
  const [mode, setMode] = useState<Mode>(prefs.mode);

  useEffect(() => {
    savePrefs({ theme, locale, mode });
    void invokeCmd("save_ui_prefs", { theme, locale, mode });
  }, [theme, locale, mode]);

  return (
    <ThemeProvider initial={theme} onChange={setTheme}>
      <I18nProvider locale={locale} setLocale={setLocale}>
        <Shell theme={theme} setTheme={setTheme} locale={locale} setLocale={setLocale} mode={mode} setMode={setMode} />
      </I18nProvider>
    </ThemeProvider>
  );
}

function Shell({
  theme,
  setTheme,
  locale,
  setLocale,
  mode,
  setMode,
}: {
  theme: ThemeId;
  setTheme: (t: ThemeId) => void;
  locale: Locale;
  setLocale: (l: Locale) => void;
  mode: Mode;
  setMode: (m: Mode) => void;
}) {
  const { t } = useI18n();
  const { setTheme: applyTheme } = useTheme();
  const [keymap, setKeymap] = useState<KeymapState>(defaultKeymapState);
  // Pending-confirm demo state — in the real app this is driven by board frames.
  const [pending, setPending] = useState<string | null>(null);

  const onApply = (roles: Role[]) => {
    void invokeCmd("set_keymap", { roles });
  };

  const chooseTheme = (id: ThemeId) => {
    setTheme(id);
    applyTheme(id);
  };

  return (
    <div className="kc-root">
      <div className="kc-shell">
        <header className="kc-header">
          <div className="kc-brand">
            <b>{t("brand.name")}</b>
            <span>{t("brand.tagline")}</span>
          </div>
          <span className="kc-pill online">
            <span className="dot" />
            {t("status.online")}
          </span>
          <div className="kc-keymap-head" style={{ margin: 0 }}>
            <button type="button" className={`kc-seg ${mode === "simple" ? "on" : ""}`} onClick={() => setMode("simple")}>
              {t("common.simple")}
            </button>
            <button type="button" className={`kc-seg ${mode === "advanced" ? "on" : ""}`} onClick={() => setMode("advanced")}>
              {t("common.advanced")}
            </button>
          </div>
          <div className="kc-settings-pop">
            <select aria-label={t("common.theme")} value={theme} onChange={(e) => chooseTheme(e.target.value as ThemeId)}>
              {THEMES.map((th) => (
                <option key={th.id} value={th.id}>
                  {th.label}
                </option>
              ))}
            </select>
            <select aria-label={t("common.language")} value={locale} onChange={(e) => setLocale(e.target.value as Locale)}>
              {LOCALES.map((l) => (
                <option key={l.id} value={l.id}>
                  {l.label}
                </option>
              ))}
            </select>
          </div>
        </header>

        {pending && (
          <div className="kc-pending">
            <b>{t("pending.title")}</b>
            <div style={{ marginTop: 6 }}>{t("pending.confirming", { text: pending })}</div>
            <div className="kc-muted" style={{ fontSize: 12, marginTop: 8, display: "flex", gap: 14, flexWrap: "wrap" }}>
              <span><LedDot color="red" effect="breathing" /> {t("pending.k4")}</span>
              <span><LedDot color="green" effect="single" /> {t("pending.k6")}</span>
            </div>
          </div>
        )}

        {mode === "simple" ? (
          <Card title={t("brand.name")}>
            <div className="kc-stack">
              <div>
                <StateBadge tone="success">{t("status.online")}</StateBadge>{" "}
                <StateBadge tone="neutral">{t("simple.voice.idle")}</StateBadge>
              </div>
              <div className="kc-muted">{t("simple.followingAll")}</div>
              <button type="button" className="kc-btn ghost" onClick={() => setMode("advanced")}>
                {t("simple.openAdvanced")}
              </button>
            </div>
          </Card>
        ) : (
          <div className="kc-stack">
            <KeymapPanel value={keymap} onChange={setKeymap} onApply={onApply} />
            {/* Dev affordance to preview the pending-confirm visualizer. */}
            <button
              type="button"
              className="kc-btn ghost"
              style={{ alignSelf: "flex-start" }}
              onClick={() => setPending(pending ? null : "你好，Kiro")}
            >
              {pending ? "hide pending-confirm" : "preview pending-confirm"}
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
