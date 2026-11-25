/* Local firmware update over USB. The backend checks the
 * package signature; the keyboard checks it again and asks for K6 / K1. */
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { useI18n } from "../i18n";

type PackageInfo = { version: string; profile: string; key: string; image_size: number };
type Progress = { state: string; acked: number; total: number; reason: string; version: string };

function toBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

export function FirmwareUpdateCard({ usb, firmware }: { usb: boolean; firmware: string }) {
  const { t } = useI18n();
  const [pkg, setPkg] = useState<PackageInfo | null>(null);
  const [progress, setProgress] = useState<Progress | null>(null);
  const [error, setError] = useState("");

  useEffect(() => {
    const unlisten = listen<Progress>("ota-progress", (event) => setProgress(event.payload));
    return () => {
      void unlisten.then((fn) => fn());
    };
  }, []);

  const running = progress !== null && !["failed", "installed"].includes(progress.state);

  async function pick(file: File | undefined) {
    setError("");
    setPkg(null);
    setProgress(null);
    if (!file) return;
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      setPkg(await invoke<PackageInfo>("ota_load", { dataB64: toBase64(bytes) }));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  async function run(cmd: "ota_start" | "ota_cancel") {
    setError("");
    try {
      await invoke(cmd);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  const percent = progress && progress.total > 0 ? Math.floor((progress.acked * 100) / progress.total) : 0;
  const stateText = progress
    ? t(`ota.state.${progress.state}`, { version: progress.version, reason: progress.reason, percent })
    : "";

  return (
    <>
      <div className="kc-muted kc-note">{t("ota.current", { version: firmware || "--" })}</div>
      <div className="kc-field">
        <input type="file" accept=".akota" disabled={!usb || running} onChange={(e) => void pick(e.target.files?.[0])} />
      </div>
      {pkg && (
        <div className="kc-muted kc-note">
          {t("ota.package", { version: pkg.version, profile: pkg.profile, key: pkg.key, size: Math.round(pkg.image_size / 1024) })}
        </div>
      )}
      <div className="kc-field">
        <button type="button" className="kc-btn" disabled={!usb || !pkg || running} onClick={() => void run("ota_start")}>
          {t("ota.install")}
        </button>
        <button type="button" className="kc-btn ghost" disabled={!running || progress?.state === "installing"} onClick={() => void run("ota_cancel")}>
          {t("ota.cancel")}
        </button>
      </div>
      {progress && (
        <div className="kc-field">
          <span className="kc-muted">OTA</span>
          <strong>{stateText}</strong>
        </div>
      )}
      {error && <div className="error-banner">{error}</div>}
      <div className="kc-muted kc-note">{t("ota.hint")}</div>
    </>
  );
}
