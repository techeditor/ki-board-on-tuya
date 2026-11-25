//! Which recognizer handles the keyboard's voice key (the `voice_engine`
//! route, mirrored to the board with a `config` frame).
//!
//! - `Cloud`: the keyboard mic and Tuya cloud ASR; the Companion only sees
//!   the resulting `asr` text.
//! - `System`: PC recognition. The keyboard sends `voice:start` / `voice:stop`
//!   and the Companion records and recognises (see `stream_asr`,
//!   docs/pc-asr.md). The earlier Win+H speech input, which typed straight into
//!   the focused window, is replaced: PC text now goes to the keyboard's ③.

/// Which ASR the system uses for a board voice event.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum VoiceSource {
    /// Board records on its own mic → Tuya cloud ASR (FP-3). Companion ignores.
    #[default]
    Cloud,
    /// PC recognition (`stream_asr`).
    System,
}

impl VoiceSource {
    /// Parse the UI/board `voice_engine` string; unknown values fall back to Cloud.
    pub fn parse(s: &str) -> VoiceSource {
        match s.trim().to_ascii_lowercase().as_str() {
            "system" | "pc" | "windows" => VoiceSource::System,
            _ => VoiceSource::Cloud,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            VoiceSource::Cloud => "cloud",
            VoiceSource::System => "system",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cloud_is_default_source() {
        assert_eq!(VoiceSource::default(), VoiceSource::Cloud);
    }

    #[test]
    fn source_parse_roundtrip() {
        assert_eq!(VoiceSource::parse("system"), VoiceSource::System);
        assert_eq!(VoiceSource::parse("SYSTEM"), VoiceSource::System);
        assert_eq!(VoiceSource::parse("cloud"), VoiceSource::Cloud);
        assert_eq!(VoiceSource::parse("bogus"), VoiceSource::Cloud);
        assert_eq!(VoiceSource::System.as_str(), "system");
        assert_eq!(VoiceSource::Cloud.as_str(), "cloud");
    }
}
