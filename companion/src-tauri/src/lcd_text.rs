//! LCD text the Companion writes to the keyboard (menu titles / labels,
//! `menu_close` messages, notices) in the keyboard's interface language
//! (FR-01, `device_settings_state.language`).
//!
//! Call sites keep the English source text and wrap it in [`lcd`]; the board's
//! language decides what is sent. Until a board reports `"zh"` (older firmware
//! never does, and cannot draw Chinese) everything stays English.

use std::sync::atomic::{AtomicBool, Ordering};

static BOARD_ZH: AtomicBool = AtomicBool::new(false);

/// Record the language the board reported (`None`: older firmware -> English).
pub fn set_board_language(language: Option<&str>) {
    BOARD_ZH.store(language == Some("zh"), Ordering::Relaxed);
}

/// Whether the board reported Chinese.
pub fn board_zh() -> bool {
    BOARD_ZH.load(Ordering::Relaxed)
}

/// `en` in the board's language.
pub fn lcd(en: &'static str) -> &'static str {
    tr(en, BOARD_ZH.load(Ordering::Relaxed))
}

/// `en` in Chinese when `zh`; text without a translation passes through.
pub fn tr(en: &'static str, zh: bool) -> &'static str {
    if !zh {
        return en;
    }
    match en {
        "Done" => "已完成",
        "Expired" => "已过期",
        "Invalid" => "无效",
        "Failed" => "失败",
        "Dictate answer" => "语音回答",
        "Approve once" => "批准一次",
        "Reject" => "拒绝",
        "Reject once" => "本次拒绝",
        "Reply" => "回复",
        "Next step" => "下一步",
        "Question" => "问题",
        "No chat selected" => "未选择对话",
        "Select a chat first" => "请先选择对话",
        "No model picked yet" => "还没有选过模型",
        "Auto" => "自动",
        "Model" => "模型",
        "Mode" => "模式",
        "Normal" => "正常",
        "Reads" => "读取免批",
        "Trust" => "信任",
        "Model list too long" => "模型列表太长",
        "Model list unavailable" => "无法获取模型列表",
        "Model change failed" => "模型切换失败",
        "Crew offline" => "Crew 离线",
        "Crew window not in front" => "Crew 窗口不在前台",
        "Approved" => "已批准",
        "Rejected" => "已拒绝",
        "Sent" => "已发送",
        "Model: next turn" => "模型：下一轮生效",
        "Model set" => "模型已切换",
        "Mode: Normal" => "模式：正常",
        "Mode: Reads" => "模式：读取免批",
        "Mode: Trust" => "模式：信任",
        "Model: " => "模型：",
        " (next turn)" => "（下一轮）",
        "Failed: " => "失败：",
        "Crew identity check failed" => "Crew 身份校验失败",
        "PC ASR: Doubao" => "电脑识别:豆包",
        "PC ASR: Aliyun" => "电脑识别:阿里云",
        "PC ASR: local" => "电脑识别:本地",
        "Cloud failed, used local" => "云端失败,已用本地识别",
        "PC ASR: no API key" => "电脑识别:没有 Key",
        "PC ASR: key refused" => "电脑识别:Key 无效",
        "PC ASR: network error" => "电脑识别:网络错误",
        "PC ASR failed" => "电脑识别失败",
        "PC ASR timed out" => "电脑识别超时",
        "Local model missing" => "本地模型未下载",
        "Microphone error" => "电脑麦克风错误",
        "Didn't catch that" => "没听清",
        "Voice needs keyboard control" => "语音需要打开键盘控制电脑",
        "Bind" => "绑定",
        "This computer" => "本机",
        "Unbind" => "解除绑定",
        "Bound" => "已绑定",
        "Unbound" => "已解除绑定",
        "Session list unavailable" => "无法获取会话列表",
        "Binding not saved" => "绑定未保存",
        "Remote chat: status only" => "远端会话：仅显示状态",
        "No chat selected (K1-K3)" => "未选择对话(K1-K3)",
        "Chat not bound (Settings)" => "这个键没有绑定对话",
        "Nothing to send" => "没有可发送的内容",
        "Crew refused: sign-in" => "Crew 拒绝：请先登录",
        "Crew not running" => "Crew 没有运行",
        "Send failed" => "发送失败",
        "Typing failed" => "输入失败，文字留在键盘",
        _ => en,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::menu_wire::{CLOSE_MSG_MAX, LABEL_MAX, NOTICE_MAX};

    #[test]
    fn english_is_the_identity_and_unknown_text_passes() {
        assert_eq!(tr("Approve once", false), "Approve once");
        assert_eq!(tr("Approve once", true), "批准一次");
        assert_eq!(tr("some API error", true), "some API error");
    }

    #[test]
    fn chinese_texts_fit_the_wire_limits() {
        for en in ["Approve once", "Reject once", "Dictate answer", "Normal", "Reads", "Trust", "Auto", "This computer", "Unbind"] {
            assert!(tr(en, true).len() <= LABEL_MAX, "{en}");
        }
        for en in ["Done", "Expired", "Invalid", "Failed", "Model: next turn", "Mode: Reads", "Approved"] {
            assert!(tr(en, true).len() <= CLOSE_MSG_MAX, "{en}");
        }
        for en in ["Crew window not in front", "Model list unavailable", "Select a chat first", "Crew identity check failed",
                   "Session list unavailable", "Binding not saved", "Remote chat: status only", "Bound", "Unbound",
                   "No chat selected (K1-K3)", "Chat not bound (Settings)", "Nothing to send", "Crew refused: sign-in",
                   "Crew not running", "Send failed", "Typing failed"] {
            assert_ne!(tr(en, true), en, "{en} has no Chinese text");
            assert!(tr(en, true).len() <= NOTICE_MAX, "{en}");
        }
    }

    #[test]
    fn pc_asr_texts_are_translated_and_fit_a_notice() {
        let errors = [
            crate::stream_asr::AsrErrorKind::NoKey,
            crate::stream_asr::AsrErrorKind::Auth,
            crate::stream_asr::AsrErrorKind::Network,
            crate::stream_asr::AsrErrorKind::Server,
            crate::stream_asr::AsrErrorKind::Timeout,
            crate::stream_asr::AsrErrorKind::NotInstalled,
            crate::stream_asr::AsrErrorKind::Audio,
        ]
        .map(|k| crate::stream_asr::AsrError::new(k, "").short());
        let labels = ["PC ASR: Doubao", "PC ASR: Aliyun", "PC ASR: local", "Cloud failed, used local", "Microphone error", "Didn't catch that", "Voice needs keyboard control"];
        for en in errors.iter().chain(labels.iter()) {
            assert!(en.len() <= NOTICE_MAX, "{en}");
            assert_ne!(tr(en, true), *en, "{en} has no Chinese text");
            assert!(tr(en, true).len() <= NOTICE_MAX, "{en}");
        }
    }

}
