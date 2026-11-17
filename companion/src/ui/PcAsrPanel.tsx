/* Voice recognition path + PC streaming ASR settings (docs/pc-asr.md).
 * API keys go straight to the system keychain through the backend; this page
 * never receives a stored key back, only whether one is stored. */
import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { useI18n } from "../i18n";

type Engine = "doubao" | "aliyun" | "local";

type AsrSettings = {
  engine: Engine;
  fallback_local: boolean;
  hotwords: string[];
  doubao_app_id: string;
  doubao_resource: string;
  aliyun_model: string;
};

type AsrView = {
  settings: AsrSettings;
  keys: Record<string, boolean>;
  local: { installed: boolean; supported: boolean; download_bytes: number; model: string; license: string; dir: string };
  downloading: boolean;
};

type AsrEvent = { id: number; state: string; engine: Engine; text: string; message: string };
type DownloadEvent = { state: "running" | "done" | "error"; done?: number; total?: number; message?: string };

const ENGINES: Engine[] = ["doubao", "aliyun", "local"];
const DOUBAO_RESOURCES = [
  "volc.seedasr.sauc.duration",
  "volc.seedasr.sauc.concurrent",
  "volc.bigasr.sauc.duration",
  "volc.bigasr.sauc.concurrent",
];
const ALIYUN_MODELS = ["fun-asr-realtime", "paraformer-realtime-v2"];

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export function PcAsrPanel({
  voiceSource,
  onSelectSource,
}: {
  voiceSource: "cloud" | "system";
  onSelectSource: (source: "cloud" | "system") => Promise<void>;
}) {
  const { t } = useI18n();
  const [view, setView] = useState<AsrView | null>(null);
  const [keyInput, setKeyInput] = useState("");
  const [hotwords, setHotwords] = useState("");
  const [appId, setAppId] = useState("");
  const [download, setDownload] = useState<DownloadEvent | null>(null);
  const [live, setLive] = useState<AsrEvent | null>(null);
  const [testing, setTesting] = useState(false);
  const [error, setError] = useState("");

  function apply(v: AsrView) {
    setView(v);
    setHotwords(v.settings.hotwords.join("\n"));
    setAppId(v.settings.doubao_app_id);
  }

  useEffect(() => {
    invoke<AsrView>("asr_get_settings").then(apply).catch((e) => setError(errorText(e)));
    const offAsr = listen<AsrEvent>("pc-asr", (e) => {
      setLive(e.payload);
      if (["final", "error"].includes(e.payload.state)) setTesting(false);
    });
    const offDl = listen<DownloadEvent>("asr-download", (e) => {
      setDownload(e.payload);
      if (e.payload.state !== "running") {
        invoke<AsrView>("asr_get_settings").then(apply).catch(() => undefined);
      }
    });
    return () => {
      void offAsr.then((f) => f());
      void offDl.then((f) => f());
    };
  }, []);

  async function call(cmd: string, args?: Record<string, unknown>) {
    setError("");
    try {
      const v = await invoke<AsrView | null>(cmd, args);
      if (v) apply(v);
    } catch (e) {
      setError(errorText(e));
    }
  }

  function save(patch: Partial<AsrSettings>) {
    if (!view) return;
    void call("asr_set_settings", { settings: { ...view.settings, ...patch } });
  }

  async function saveKey(engine: Engine) {
    await call("asr_set_key", { engine, key: keyInput });
    setKeyInput("");
  }

  async function toggleTest() {
    setError("");
    try {
      if (testing) {
        await invoke("asr_test_stop");
      } else {
        setLive(null);
        await invoke("asr_test_start");
        setTesting(true);
      }
    } catch (e) {
      setError(errorText(e));
    }
  }

  const s = view?.settings;
  const engine = s?.engine ?? "doubao";
  const hasKey = view?.keys[engine] ?? false;
  const mb = (n: number) => Math.round(n / 1_000_000);
  const dlPercent = download?.total ? Math.floor(((download.done ?? 0) * 100) / download.total) : 0;
  const downloading = view?.downloading || download?.state === "running";

  return (
    <>
      <label>{t("asr.route")}</label>
      <div className="kc-keymap-head" style={{ marginTop: 6, marginBottom: 6 }}>
        {(["cloud", "system"] as const).map((src) => (
          <button
            key={src}
            type="button"
            className={`kc-seg ${voiceSource === src ? "on" : ""}`}
            onClick={() => void onSelectSource(src).catch((e) => setError(errorText(e)))}
          >
            {t(`asr.route.${src}`)}
          </button>
        ))}
      </div>
      <p className="hint">{t(`asr.route.${voiceSource}.hint`)}</p>

      {voiceSource === "system" && s && view && (
        <>
          <label style={{ marginTop: 10 }}>{t("asr.engine")}</label>
          <div className="kc-keymap-head" style={{ marginTop: 6, marginBottom: 6 }}>
            {ENGINES.map((e) => (
              <button key={e} type="button" className={`kc-seg ${engine === e ? "on" : ""}`} onClick={() => save({ engine: e })}>
                {t(`asr.engine.${e}`)}
                {e !== "local" && (view.keys[e] ? " ✓" : "")}
                {e === "local" && (view.local.installed ? " ✓" : "")}
              </button>
            ))}
          </div>

          {engine !== "local" && (
            <>
              <p className={`hint ${hasKey ? "good-text" : "warn-text"}`}>
                {t(hasKey ? "asr.key.stored" : "asr.key.missing")} {t(`asr.key.where.${engine}`)}
              </p>
              <div className="actions" style={{ marginTop: 4 }}>
                <input
                  type="password"
                  autoComplete="off"
                  value={keyInput}
                  placeholder={t(hasKey ? "asr.key.replace" : "asr.key.placeholder")}
                  onChange={(e) => setKeyInput(e.target.value)}
                />
                <button disabled={keyInput.trim().length === 0} onClick={() => void saveKey(engine)}>
                  {t("asr.key.save")}
                </button>
                <button className="secondary" disabled={!hasKey} onClick={() => void call("asr_clear_key", { engine })}>
                  {t("asr.key.clear")}
                </button>
              </div>
            </>
          )}

          {engine === "doubao" && (
            <div className="kc-field">
              <span>{t("asr.doubao.resource")}</span>
              <select value={s.doubao_resource} onChange={(e) => save({ doubao_resource: e.target.value })}>
                {DOUBAO_RESOURCES.map((r) => (
                  <option key={r} value={r}>
                    {t(`asr.doubao.resource.${r}`)}
                  </option>
                ))}
              </select>
              <span>{t("asr.doubao.appId")}</span>
              <input
                value={appId}
                placeholder={t("asr.doubao.appId.placeholder")}
                onChange={(e) => setAppId(e.target.value)}
                onBlur={() => appId !== s.doubao_app_id && save({ doubao_app_id: appId })}
              />
            </div>
          )}

          {engine === "aliyun" && (
            <div className="kc-field">
              <span>{t("asr.aliyun.model")}</span>
              <select value={s.aliyun_model} onChange={(e) => save({ aliyun_model: e.target.value })}>
                {ALIYUN_MODELS.map((m) => (
                  <option key={m} value={m}>
                    {t(`asr.aliyun.model.${m}`)}
                  </option>
                ))}
              </select>
            </div>
          )}

          {(engine === "local" || s.fallback_local) && (
            <>
              <p className={`hint ${view.local.installed ? "good-text" : "warn-text"}`} style={{ marginTop: 8 }}>
                {view.local.installed
                  ? t("asr.local.ready")
                  : t("asr.local.missing", { mb: mb(view.local.download_bytes) })}
              </p>
              {!view.local.supported && <p className="hint warn-text">{t("asr.local.unsupported")}</p>}
              <div className="actions" style={{ marginTop: 4 }}>
                {!view.local.installed && (
                  <button disabled={downloading || !view.local.supported} onClick={() => void call("asr_local_download")}>
                    {t("asr.local.download", { mb: mb(view.local.download_bytes) })}
                  </button>
                )}
                {downloading && (
                  <button className="secondary" onClick={() => void call("asr_local_cancel")}>
                    {t("asr.local.cancel")}
                  </button>
                )}
                {view.local.installed && (
                  <button className="secondary" onClick={() => void call("asr_local_remove")}>
                    {t("asr.local.remove")}
                  </button>
                )}
              </div>
              {downloading && <p className="hint">{t("asr.local.progress", { percent: dlPercent })}</p>}
              {download?.state === "error" && <p className="hint warn-text">{download.message}</p>}
              <p className="hint">{t("asr.local.about", { model: view.local.model, license: view.local.license, dir: view.local.dir })}</p>
            </>
          )}

          <label style={{ marginTop: 10 }}>
            <input
              type="checkbox"
              checked={s.fallback_local}
              onChange={(e) => save({ fallback_local: e.target.checked })}
              style={{ width: "auto", marginRight: 6 }}
            />
            {t("asr.fallback")}
          </label>

          <label style={{ marginTop: 10 }}>{t("asr.hotwords")}</label>
          <textarea
            rows={3}
            value={hotwords}
            placeholder={t("asr.hotwords.placeholder")}
            onChange={(e) => setHotwords(e.target.value)}
            onBlur={() => save({ hotwords: hotwords.split(/[\n,，、]/) })}
          />
          <p className="hint">{t("asr.hotwords.hint")}</p>

          <div className="actions" style={{ marginTop: 8 }}>
            <button className={testing ? "" : "secondary"} onClick={() => void toggleTest()}>
              {t(testing ? "asr.test.stop" : "asr.test.start")}
            </button>
          </div>
          {live && (
            <div className="lcd" style={{ marginTop: 6 }}>
              <span>{t(`asr.engine.${live.engine}`)}</span>
              <strong className={live.state === "final" ? "good-text" : live.state === "error" ? "warn-text" : ""}>
                {live.text || t(`asr.state.${live.state}`, { message: t(live.message) })}
              </strong>
            </div>
          )}
          <p className="hint">{t("asr.test.hint")}</p>
        </>
      )}
      {error && <p className="hint warn-text">{error}</p>}
    </>
  );
}
