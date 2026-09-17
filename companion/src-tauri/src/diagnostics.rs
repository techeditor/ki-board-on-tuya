//! Persistent diagnostics for hangs and crashes in the packaged Companion.
//!
//! Records only lifecycle and timing metadata. Callers must not pass protocol
//! payloads, recognized speech, Wi-Fi passwords, or license values.

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, Once, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

const LOG_DIR: &str = "logs";
const LOG_FILE: &str = "companion.log";
const OLD_LOG_FILE: &str = "companion.previous.log";
const MAX_LOG_BYTES: u64 = 4 * 1024 * 1024;

struct Logger {
    path: PathBuf,
    file: Mutex<Option<File>>,
}

static LOGGER: OnceLock<Logger> = OnceLock::new();
static PANIC_HOOK: Once = Once::new();

/// Diagnostic logs live at `~/.ak-companion/logs/companion.log` (see
/// `app_dirs`), alongside the bind store and hook handoff.
fn log_path() -> PathBuf {
    crate::app_dirs::companion_dir()
        .join(LOG_DIR)
        .join(LOG_FILE)
}

fn open_logger() -> Logger {
    let path = log_path();
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if path.metadata().map(|m| m.len()).unwrap_or(0) >= MAX_LOG_BYTES {
        let previous = path.with_file_name(OLD_LOG_FILE);
        let _ = fs::remove_file(&previous);
        let _ = fs::rename(&path, previous);
    }
    let file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .ok();
    Logger {
        path,
        file: Mutex::new(file),
    }
}

fn logger() -> &'static Logger {
    LOGGER.get_or_init(open_logger)
}

fn redact_assignment(input: &mut String, key: &str) {
    let mut search_from = 0;
    loop {
        let lower = input[search_from..].to_ascii_lowercase();
        let Some(relative_start) = lower.find(key) else {
            break;
        };
        let start = search_from + relative_start;
        let value_start = start + key.len();
        let value_end = input[value_start..]
            .find(|c: char| c.is_whitespace() || matches!(c, ',' | '}' | ']'))
            .map(|offset| value_start + offset)
            .unwrap_or(input.len());
        input.replace_range(value_start..value_end, "<redacted>");
        search_from = value_start + "<redacted>".len();
    }
}

fn sanitize(message: &str) -> String {
    let mut safe = message.replace(['\r', '\n'], " ");
    for key in ["authkey=", "uuid=", "password=", "pass="] {
        redact_assignment(&mut safe, key);
    }
    safe
}

/// Initialize the log file and panic hook. Safe to call more than once.
pub fn init() -> PathBuf {
    let path = logger().path.clone();
    PANIC_HOOK.call_once(|| {
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            record("FATAL", "panic", &info.to_string());
            previous(info);
        }));
    });
    record("INFO", "lifecycle", "companion process starting");
    record(
        "INFO",
        "lifecycle",
        &format!("diagnostic log path={}", path.display()),
    );
    path
}

/// Append and flush one diagnostic record, and mirror it to stderr in dev runs.
pub fn record(level: &str, component: &str, message: &str) {
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let current = std::thread::current();
    let thread = current.name().unwrap_or("unnamed");
    let line = format!(
        "{now_ms} [{level}] [{component}] [{thread}] {}\n",
        sanitize(message)
    );
    eprint!("{line}");
    if let Ok(mut guard) = logger().file.lock() {
        if let Some(file) = guard.as_mut() {
            let _ = file.write_all(line.as_bytes());
            let _ = file.flush();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitizes_secrets_and_multiline_messages() {
        assert_eq!(
            sanitize("uuid=abc authkey=secret pass=hunter2\nnext"),
            "uuid=<redacted> authkey=<redacted> pass=<redacted> next"
        );
    }
}
