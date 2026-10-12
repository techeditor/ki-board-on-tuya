/* i18n (R2). Every user-facing string resolves through t(key, vars?); no inline
 * literals in components. Fallback chain: selected locale → en → key name. */
import { createContext, useContext, type ReactNode } from "react";

import en from "./en.json";
import zh from "./zh.json";

export type Locale = "en" | "zh";

const CATALOGS: Record<Locale, Record<string, string>> = { en, zh };

export const LOCALES: { id: Locale; label: string }[] = [
  { id: "en", label: "English" },
  { id: "zh", label: "中文" },
];

const DEFAULT_LOCALE: Locale = "zh";

export function isLocale(v: unknown): v is Locale {
  return v === "en" || v === "zh";
}

/** Resolve a key with {var} interpolation; falls back locale → en → key. */
export function translate(locale: Locale, key: string, vars?: Record<string, string | number>): string {
  const raw = CATALOGS[locale]?.[key] ?? CATALOGS.en[key] ?? key;
  if (!vars) return raw;
  return raw.replace(/\{(\w+)\}/g, (_m, name: string) =>
    name in vars ? String(vars[name]) : `{${name}}`,
  );
}

type I18nCtx = { locale: Locale; setLocale: (l: Locale) => void; t: (key: string, vars?: Record<string, string | number>) => string };
const Ctx = createContext<I18nCtx>({
  locale: DEFAULT_LOCALE,
  setLocale: () => {},
  t: (k) => translate(DEFAULT_LOCALE, k),
});

export function useI18n(): I18nCtx {
  return useContext(Ctx);
}

export function I18nProvider({
  locale,
  setLocale,
  children,
}: {
  locale: Locale;
  setLocale: (l: Locale) => void;
  children: ReactNode;
}) {
  const t = (key: string, vars?: Record<string, string | number>) => translate(locale, key, vars);
  return <Ctx.Provider value={{ locale, setLocale, t }}>{children}</Ctx.Provider>;
}
