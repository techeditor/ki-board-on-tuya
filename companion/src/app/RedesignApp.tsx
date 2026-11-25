/* Companion shell (design §1, §5). Header with brand, board status, the
 * Simple/Advanced mode switch and a settings popover; Simple shows connection,
 * voice and the watched chat; Advanced has a left nav with Connection, Input,
 * Control and Logs. The mode, theme, language and control mechanism persist
 * to localStorage (key ak-ui-prefs, same as the previous entry). */
import { useEffect, useMemo, useState } from "react";

import { ThemeProvider, THEMES, useTheme, type ThemeId } from "../theme";
import { I18nProvider, LOCALES, isLocale, type Locale } from "../i18n";
import { Card, StateBadge } from "../ui/primitives";
import { SimpleVoiceLine } from "../ui/BoardStatusCards";
import { sessionDisplayName } from "./companionTypes";
import { useCompanion, tauriReady, type Model } from "./useCompanion";
import { useAppVersion } from "./useAppVersion";
import { ConnectionView } from "./views/ConnectionView";
import { InputView } from "./views/InputView";
import { ControlView, type Mechanism } from "./views/ControlView";
import { LogsView } from "./views/LogsView";
import { DeviceSettingsView } from "./views/DeviceSettingsView";
import { AdvancedSettingsView } from "./views/AdvancedSettingsView";
import { SceneQuickSwitch, ScenesPanel, useScenes, type ScenesModel } from "../ui/ScenesPanel";

import "../theme/components.css";

type Mode = "simple" | "advanced";
type View = "connection" | "input" | "scenes" | "control" | "deviceSettings" | "advancedSettings" | "logs";
const VIEWS: View[] = ["connection", "input", "scenes", "control", "deviceSettings", "advancedSettings", "logs"];

interface UiPrefs {
  theme: ThemeId;
  locale: Locale;
  mode: Mode;
  mechanism: Mechanism;
}

const PREFS_KEY = "ak-ui-prefs";

function loadPrefs(): UiPrefs {
  try {
    const raw = localStorage.getItem(PREFS_KEY);
    if (raw) {
      const p = JSON.parse(raw) as Partial<UiPrefs>;
      return {
        theme: THEMES.some((th) => th.id === p.theme) ? (p.theme as ThemeId) : "kc-dark",
        locale: isLocale(p.locale) ? p.locale : "zh",
        mode: p.mode === "advanced" ? "advanced" : "simple",
        mechanism: p.mechanism === "legacy" ? "legacy" : "api",
      };
    }
  } catch {
    /* malformed prefs fall back to defaults */
  }
  return { theme: "kc-dark", locale: "zh", mode: "simple", mechanism: "api" };
}

function savePrefs(p: UiPrefs): void {
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(p));
  } catch {
    /* storage unavailable: prefs are per session only */
  }
}

export function RedesignApp() {
  const prefs = useMemo(loadPrefs, []);
  const [theme, setTheme] = useState<ThemeId>(prefs.theme);
  const [locale, setLocale] = useState<Locale>(prefs.locale);
  const [mode, setMode] = useState<Mode>(prefs.mode);
  const [mechanism, setMechanism] = useState<Mechanism>(prefs.mechanism);

  useEffect(() => {
    savePrefs({ theme, locale, mode, mechanism });
  }, [theme, locale, mode, mechanism]);

  return (
    <ThemeProvider initial={theme} onChange={setTheme}>
      <I18nProvider locale={locale} setLocale={setLocale}>
        <Shell mode={mode} setMode={setMode} mechanism={mechanism} setMechanism={setMechanism} />
      </I18nProvider>
    </ThemeProvider>
  );
}

function Shell({
  mode,
  setMode,
  mechanism,
  setMechanism,
}: {
  mode: Mode;
  setMode: (m: Mode) => void;
  mechanism: Mechanism;
  setMechanism: (m: Mechanism) => void;
}) {
  const m = useCompanion();
  const { t, locale } = m;
  const { theme, setTheme } = useTheme();
  const [view, setView] = useState<View>("connection");
  const [settingsOpen, setSettingsOpen] = useState(false);
  const preview = typeof window !== "undefined" && new URLSearchParams(window.location.search).has("preview");
  const online = m.status.device_connected;
  const appVersion = useAppVersion();
  const sc = useScenes();

  return (
    <div className="kc-root">
      <div className="kc-shell">
        <header className="kc-header">
          <div className="kc-brand">
            <b>
              {t("brand.name")} <span className="kc-ver" title={t("about.companion")}>v{appVersion}</span>
            </b>
            <span>{t("brand.tagline")}</span>
          </div>
          <span className={`kc-pill ${online ? "online" : ""}`}>
            <span className="dot" />
            {online ? `${t("status.online")} · ${m.status.transport.toUpperCase()}` : t("status.offline")}
          </span>
          {online && m.status.handshake_missing && (
            <span className="kc-pill warn" role="status">
              <span className="dot" />
              {t("status.noHandshake")}
            </span>
          )}
          <div className="kc-keymap-head" style={{ margin: 0 }}>
            <button type="button" className={`kc-seg ${mode === "simple" ? "on" : ""}`} onClick={() => setMode("simple")}>
              {t("common.simple")}
            </button>
            <button type="button" className={`kc-seg ${mode === "advanced" ? "on" : ""}`} onClick={() => setMode("advanced")}>
              {t("common.advanced")}
            </button>
          </div>
          <div className="kc-settings">
            <button
              type="button"
              className="kc-btn ghost"
              aria-label={t("common.settings")}
              aria-expanded={settingsOpen}
              onClick={() => setSettingsOpen((v) => !v)}
            >
              <svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor" strokeWidth="2">
                <circle cx="12" cy="12" r="3" />
                <path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z" />
              </svg>
            </button>
            {settingsOpen && (
              <div className="kc-popover" role="dialog" aria-label={t("common.settings")}>
                <div className="kc-field" role="group" aria-label={t("common.theme")} style={{ marginTop: 0 }}>
                  <span>{t("common.theme")}</span>
                  <div className="kc-keymap-head kc-setting-choices">
                    {THEMES.map((th) => (
                      <button
                        key={th.id}
                        type="button"
                        className={`kc-seg ${theme === th.id ? "on" : ""}`}
                        aria-pressed={theme === th.id}
                        onClick={() => setTheme(th.id)}
                      >
                        {themeLabel(t, th.id, th.label)}
                      </button>
                    ))}
                  </div>
                </div>
                <div className="kc-field" role="group" aria-label={t("common.language")}>
                  <span>{t("common.language")}</span>
                  <div className="kc-keymap-head kc-setting-choices">
                    {LOCALES.map((l) => (
                      <button
                        key={l.id}
                        type="button"
                        className={`kc-seg ${locale === l.id ? "on" : ""}`}
                        aria-pressed={locale === l.id}
                        onClick={() => m.chooseLocale(l.id)}
                      >
                        {l.label}
                      </button>
                    ))}
                  </div>
                </div>
                <div className="kc-about" aria-label={t("about.title")}>
                  <div className="kc-about-title">{t("about.title")}</div>
                  <div className="kc-about-row">
                    <span>{t("about.companion")}</span>
                    <b>v{appVersion}</b>
                  </div>
                  {online && (
                    <div className="kc-about-row">
                      <span>{t("about.firmware")}</span>
                      <b>{m.status.firmware_version ? `v${m.status.firmware_version.replace(/^v/i, "")}` : "--"}</b>
                    </div>
                  )}
                </div>
              </div>
            )}
          </div>
        </header>

        {mode === "simple" ? (
          <SimpleView m={m} sc={sc} onAdvanced={() => setMode("advanced")} />
        ) : (
          <div className="kc-adv">
            <nav className="kc-nav kc-nav-rail" aria-label={t("common.navigation")}>
              {VIEWS.map((v) => (
                <button key={v} type="button" className={view === v ? "on" : ""} onClick={() => setView(v)}>
                  {t(`nav.${v}`)}
                </button>
              ))}
            </nav>
            <div className="kc-adv-content">
              <h3 className="kc-view-title">{t(`nav.${view}`)}</h3>
              {m.uiError && <div className="error-banner">{m.uiError}</div>}
              {view === "connection" && <ConnectionView m={m} />}
              {view === "input" && <InputView m={m} preview={preview} />}
              {view === "scenes" && <ScenesPanel sc={sc} />}
              {view === "control" && <ControlView m={m} mechanism={mechanism} setMechanism={setMechanism} />}
              {view === "deviceSettings" && <DeviceSettingsView m={m} />}
              {view === "advancedSettings" && <AdvancedSettingsView m={m} />}
              {view === "logs" && <LogsView m={m} />}
            </div>
          </div>
        )}

        <footer className="kc-foot">
          <span>{t("footer.protocol")}</span>
          <span>v{appVersion}{tauriReady() ? "" : ` · ${t("footer.preview")}`}</span>
        </footer>
      </div>
    </div>
  );
}

/** Theme names come from the catalog; an unlisted skin falls back to its own label. */
function themeLabel(t: (key: string) => string, id: string, fallback: string): string {
  const key = `theme.${id}`;
  const text = t(key);
  return text === key ? fallback : text;
}

function SimpleView({ m, sc, onAdvanced }: { m: Model; sc: ScenesModel; onAdvanced: () => void }) {
  const { t } = m;
  const online = m.status.device_connected;
  const watching = m.pinnedSession ? sessionDisplayName(m.pinnedSession) : t("simple.followingAll");
  return (
    <div className="kc-stack">
      <Card title={t("nav.connection")}>
        <div className="kc-stack">
          <div className="kc-field" style={{ marginTop: 0 }}>
            <StateBadge tone={online ? "success" : "neutral"}>{online ? t("status.online") : t("status.offline")}</StateBadge>
            {online && <span className="kc-muted">{m.status.transport.toUpperCase()}</span>}
          </div>
          <div className="kc-field">
            <button type="button" className="kc-btn" onClick={() => m.runAction(m.connect)} disabled={online}>{t("common.connect")}</button>
            <button type="button" className="kc-btn ghost" onClick={() => m.runAction(m.disconnect)} disabled={!online}>{t("common.disconnect")}</button>
          </div>
          {m.uiError && <div className="error-banner">{m.uiError}</div>}
        </div>
      </Card>
      <div className="kc-metrics">
        <div className="kc-metric">
          <span>{t("conn.metric.voice")}</span>
          <b>{m.voiceLabel}</b>
        </div>
        <div className="kc-metric">
          <span>{t("simple.watchingLabel")}</span>
          <b className={m.pinnedSession ? "kc-ok" : ""}>{watching}</b>
        </div>
      </div>
      <SceneQuickSwitch sc={sc} />
      <SimpleVoiceLine status={m.board} />
      <button type="button" className="kc-btn ghost" onClick={onAdvanced}>{t("simple.openAdvanced")}</button>
    </div>
  );
}
