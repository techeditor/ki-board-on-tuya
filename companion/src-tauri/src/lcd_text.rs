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
        for en in ["Approve once", "Reject once", "Dictate answer", "Normal", "Reads", "Trust", "Auto"] {
            assert!(tr(en, true).len() <= LABEL_MAX, "{en}");
        }
        for en in ["Done", "Expired", "Invalid", "Failed", "Model: next turn", "Mode: Reads", "Approved"] {
            assert!(tr(en, true).len() <= CLOSE_MSG_MAX, "{en}");
        }
        for en in ["Crew window not in front", "Model list unavailable", "Select a chat first"] {
            assert!(tr(en, true).len() <= NOTICE_MAX, "{en}");
        }
    }
}
