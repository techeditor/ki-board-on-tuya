/* The Companion version shown in the UI. Read from the Tauri app version
 * (tauri.conf.json, kept in sync with companion/VERSION).
 * The browser preview has no Tauri runtime, so it falls back to the VERSION
 * value baked in at build time (vite define). */
import { useEffect, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";

declare const __APP_VERSION__: string;

function tauriAvailable(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

export function useAppVersion(): string {
  const [version, setVersion] = useState(__APP_VERSION__);
  useEffect(() => {
    if (!tauriAvailable()) return;
    getVersion()
      .then(setVersion)
      .catch(() => undefined);
  }, []);
  return version;
}
