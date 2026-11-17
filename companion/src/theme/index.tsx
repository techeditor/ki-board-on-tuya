/* Theme registry + provider (R1). A theme is a token block registered here by
 * id; switching sets `data-theme` on the document root and persists the choice.
 * Components never branch on theme — they read tokens, so adding a skin is one
 * CSS file + one THEMES entry, zero component edits (R1.4). */
import { createContext, useContext, useEffect, useState, type ReactNode } from "react";

import "./kc-dark.css";
import "./kc-light.css";

export type ThemeId = "kc-dark" | "kc-light";

export const THEMES: { id: ThemeId; label: string }[] = [
  { id: "kc-dark", label: "Kiro Dark" },
  { id: "kc-light", label: "Kiro Light" },
];

const DEFAULT_THEME: ThemeId = "kc-dark";

function isThemeId(v: unknown): v is ThemeId {
  return typeof v === "string" && THEMES.some((t) => t.id === v);
}

type ThemeCtx = { theme: ThemeId; setTheme: (t: ThemeId) => void };
const Ctx = createContext<ThemeCtx>({ theme: DEFAULT_THEME, setTheme: () => {} });

export function useTheme(): ThemeCtx {
  return useContext(Ctx);
}

/** Apply the theme to the document root so every token resolves. */
function applyTheme(theme: ThemeId): void {
  document.documentElement.setAttribute("data-theme", theme);
}

export function ThemeProvider({
  initial,
  onChange,
  children,
}: {
  initial?: ThemeId;
  onChange?: (t: ThemeId) => void;
  children: ReactNode;
}) {
  const [theme, setThemeState] = useState<ThemeId>(isThemeId(initial) ? initial : DEFAULT_THEME);

  useEffect(() => {
    applyTheme(theme);
  }, [theme]);

  const setTheme = (t: ThemeId) => {
    if (!isThemeId(t)) return;
    setThemeState(t);
    onChange?.(t);
  };

  return <Ctx.Provider value={{ theme, setTheme }}>{children}</Ctx.Provider>;
}
