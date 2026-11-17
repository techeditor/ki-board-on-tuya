//! PC ASR settings that are not secret, persisted in
//! `~/.ak-companion/asr.json`. API keys are never here: they live in the
//! system keychain (see `secrets`).

use serde::{Deserialize, Serialize};

use super::EngineKind;
use crate::app_dirs::companion_file;

/// Most hotwords kept, and the longest hotword, in characters. Doubao's
/// direct hotwords allow about 100 tokens on the streaming interface.
pub const HOTWORDS_MAX: usize = 50;
pub const HOTWORD_MAX_CHARS: usize = 20;

/// Doubao resource ids (which product the key is enabled for).
pub const DOUBAO_RESOURCES: [&str; 4] = [
    "volc.seedasr.sauc.duration",
    "volc.seedasr.sauc.concurrent",
    "volc.bigasr.sauc.duration",
    "volc.bigasr.sauc.concurrent",
];

/// Model Studio realtime models. Only the Fun-ASR one takes hotwords
/// directly (as context); Paraformer needs a vocabulary id created first.
pub const ALIYUN_MODELS: [&str; 2] = ["fun-asr-realtime", "paraformer-realtime-v2"];

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct AsrSettings {
    /// The engine the voice key uses.
    pub engine: EngineKind,
    /// Use the local engine when a cloud engine has no key or fails.
    pub fallback_local: bool,
    /// Optional words to recognise better (one entry per word or phrase).
    pub hotwords: Vec<String>,
    /// Old Volcengine console only: the App ID sent with the access token.
    /// Empty = new console (one API key).
    pub doubao_app_id: String,
    pub doubao_resource: String,
    pub aliyun_model: String,
}

impl Default for AsrSettings {
    fn default() -> Self {
        Self {
            engine: EngineKind::Doubao,
            fallback_local: true,
            hotwords: Vec::new(),
            doubao_app_id: String::new(),
            doubao_resource: DOUBAO_RESOURCES[0].to_string(),
            aliyun_model: ALIYUN_MODELS[0].to_string(),
        }
    }
}

impl AsrSettings {
    /// Clean every field; anything invalid falls back to its default.
    pub fn normalize(&mut self) {
        let mut seen = std::collections::HashSet::new();
        self.hotwords = std::mem::take(&mut self.hotwords)
            .into_iter()
            .map(|w| w.trim().to_string())
            .filter(|w| !w.is_empty() && w.chars().count() <= HOTWORD_MAX_CHARS && !w.chars().any(char::is_control))
            .filter(|w| seen.insert(w.clone()))
            .take(HOTWORDS_MAX)
            .collect();
        let app_id = self.doubao_app_id.trim();
        self.doubao_app_id = if app_id.len() <= 32 && app_id.chars().all(|c| c.is_ascii_alphanumeric()) {
            app_id.to_string()
        } else {
            String::new()
        };
        if !DOUBAO_RESOURCES.contains(&self.doubao_resource.as_str()) {
            self.doubao_resource = DOUBAO_RESOURCES[0].to_string();
        }
        if !ALIYUN_MODELS.contains(&self.aliyun_model.as_str()) {
            self.aliyun_model = ALIYUN_MODELS[0].to_string();
        }
    }

    /// Hotwords from the settings text box: one per line (commas also split).
    pub fn parse_hotwords(text: &str) -> Vec<String> {
        text.split(['\n', ',', '，', '、']).map(str::to_string).collect()
    }

    pub fn load() -> Self {
        let mut s = std::fs::read_to_string(companion_file("asr.json"))
            .ok()
            .and_then(|s| serde_json::from_str::<AsrSettings>(&s).ok())
            .unwrap_or_default();
        s.normalize();
        s
    }

    pub fn save(&self) -> Result<(), String> {
        let path = companion_file("asr.json");
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("create {}: {e}", parent.display()))?;
        }
        let body = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(&path, body).map_err(|e| format!("write {}: {e}", path.display()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_and_partial_files() {
        let s: AsrSettings = serde_json::from_str(r#"{"engine":"local"}"#).unwrap();
        assert_eq!(s.engine, EngineKind::Local);
        assert!(s.fallback_local);
        assert_eq!(s.doubao_resource, "volc.seedasr.sauc.duration");
        assert!(serde_json::from_str::<AsrSettings>(r#"{"engine":"system"}"#).is_err());
    }

    #[test]
    fn normalize_cleans_every_field() {
        let mut s = AsrSettings {
            hotwords: AsrSettings::parse_hotwords(" Kiro \nKiro,Ki-board，涂鸦\n\n\u{7}bad\n这是一个超过二十个字符的非常非常非常长的热词啊"),
            doubao_app_id: "12ab; rm".into(),
            doubao_resource: "nope".into(),
            aliyun_model: "gpt".into(),
            ..AsrSettings::default()
        };
        s.normalize();
        assert_eq!(s.hotwords, vec!["Kiro", "Ki-board", "涂鸦"]);
        assert_eq!(s.doubao_app_id, "");
        assert_eq!(s.doubao_resource, DOUBAO_RESOURCES[0]);
        assert_eq!(s.aliyun_model, ALIYUN_MODELS[0]);

        let mut many = AsrSettings { hotwords: (0..80).map(|i| format!("w{i}")).collect(), ..Default::default() };
        many.normalize();
        assert_eq!(many.hotwords.len(), HOTWORDS_MAX);
    }

    #[test]
    fn no_key_field_can_exist() {
        let json = serde_json::to_string(&AsrSettings::default()).unwrap();
        assert!(!json.to_lowercase().contains("key"), "{json}");
    }
}
