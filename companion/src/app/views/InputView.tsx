/* Input (design §1): Kiro Crew control switch, live key visualizer, voice
 * text target, PC ASR and the key mapping. Live keys mirror the board only. */
import { useState } from "react";

import { Card, StateBadge } from "../../ui/primitives";
import { PcAsrPanel } from "../../ui/PcAsrPanel";
import { LiveKeys } from "../../ui/LiveKeys";
import { KeymapPanel, defaultKeymapState, type KeymapState } from "../../ui/KeymapPanel";
import type { Model } from "../useCompanion";



export function InputView({ m, preview }: { m: Model; preview: boolean }) {
  const t = m.t;
  const status = m.status;
  const [keymap, setKeymap] = useState<KeymapState>(defaultKeymapState);
  const [pending, setPending] = useState(false);

  const control = status.control_enabled !== false;
  const focus = status.focus_on_select !== false;
  const showWorkers = status.show_workers === true;

  return (
    <div className="kc-stack">
      <Card title={t("input.control.title")} info="help.input.control">
        <div className="kc-field" style={{ marginTop: 0 }}>
          <span>{t("input.control.label")}</span>
          <StateBadge tone={control ? "success" : "neutral"}>{control ? t("common.on") : t("common.off")}</StateBadge>
          <div className="kc-keymap-head" style={{ marginBottom: 0, marginLeft: "auto" }}>
            <button type="button" className={`kc-seg ${control ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleControl(true))}>{t("common.on")}</button>
            <button type="button" className={`kc-seg ${!control ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleControl(false))}>{t("common.off")}</button>
          </div>
        </div>
        <div className="kc-field">
          <span>{t("control.focusOnSelect")}</span>
          <div className="kc-keymap-head" style={{ marginBottom: 0, marginLeft: "auto" }}>
            <button type="button" className={`kc-seg ${focus ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleFocusOnSelect(true))}>{t("common.on")}</button>
            <button type="button" className={`kc-seg ${!focus ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleFocusOnSelect(false))}>{t("common.off")}</button>
          </div>
        </div>
        <div className="kc-field">
          <span>{t("control.showWorkers")}</span>
          <div className="kc-keymap-head" style={{ marginBottom: 0, marginLeft: "auto" }}>
            <button type="button" className={`kc-seg ${showWorkers ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleShowWorkers(true))}>{t("common.on")}</button>
            <button type="button" className={`kc-seg ${!showWorkers ? "on" : ""}`} onClick={() => m.runAction(() => m.toggleShowWorkers(false))}>{t("common.off")}</button>
          </div>
        </div>
      </Card>

      <Card title={t("input.live.title")} info="help.input.live">
        <LiveKeys live={m.live} />
        <div className="kc-muted kc-note">{t("input.live.voice", { state: m.voiceLabel })}</div>
        <div className="kc-muted kc-note">{t("input.live.hint")}</div>
      </Card>

      <Card title={t("input.voice.title")} info="help.input.voice">
        <div className="kc-field" style={{ marginTop: 0 }}>
          <span>{t("control.voiceTarget")}</span>
        </div>
        <div className="kc-keymap-head">
          <button type="button" className={`kc-seg ${!status.voice_force_keyboard ? "on" : ""}`} onClick={() => m.runAction(() => m.setVoiceForceKeyboard(false))}>
            {t("control.voiceTarget.auto")}
          </button>
          <button type="button" className={`kc-seg ${status.voice_force_keyboard ? "on" : ""}`} onClick={() => m.runAction(() => m.setVoiceForceKeyboard(true))}>
            {t("control.voiceTarget.keyboard")}
          </button>
        </div>
        <p className="kc-muted kc-note">{t(status.voice_force_keyboard ? "control.voiceTarget.keyboardHint" : "control.voiceTarget.autoHint")}</p>
        <PcAsrPanel voiceSource={m.voiceSource} onSelectSource={m.selectVoiceSource} />
      </Card>

      <KeymapPanel
        value={keymap}
        onChange={setKeymap}
        onApply={(state) => m.runAction(() => m.applyKeymap(state))}
      />

      {preview && (
        <button type="button" className="kc-btn ghost" style={{ alignSelf: "flex-start" }} onClick={() => setPending((p) => !p)}>
          {pending ? "hide pending-confirm (preview)" : "preview pending-confirm (preview)"}
        </button>
      )}
      {preview && pending && (
        <div className="kc-pending">
          <b>{t("pending.title")}</b>
          <div style={{ marginTop: 6 }}>{t("pending.confirming", { text: "你好，Kiro Crew" })}</div>
        </div>
      )}
    </div>
  );
}
