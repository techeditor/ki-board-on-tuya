//! Install AK Companion status hooks using Kiro Crew's current standalone v1
//! hook format.
//!
//! Kiro Crew manages and may restore built-in agent JSON files on startup, so
//! those files are not a durable extension point. The supported user-level
//! location is `~/.kiro/crew/hooks/*.json` (or `$KIRO_HOME/crew/hooks/*.json`).
//! A single global file covers every Crew agent and activates without restart.

use std::path::{Path, PathBuf};

use serde::Serialize;
use serde_json::{json, Value};

pub const HOOK_FILE_NAME: &str = "ak-companion-status.json";
pub const HOOK_NAME_PREFIX: &str = "ak-companion-status-";
pub const HOOK_MARKER: &str = "--hook";
pub const HOOK_TRIGGERS: [(&str, &str); 4] = [
    ("SessionStart", "session-start"),
    ("UserPromptSubmit", "prompt-submit"),
    ("PostToolUse", "post-tool-use"),
    ("Stop", "stop"),
];

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct HookStatus {
    /// Command written into every AK Companion hook action.
    pub command: String,
    /// Resolved user-level Kiro Crew hooks directory.
    pub hooks_dir: String,
    /// Dedicated standalone hook file managed by Companion.
    pub hook_file: String,
    /// Number of required lifecycle triggers currently installed (0..=4).
    pub installed_events: u8,
    pub fully_installed: bool,
    pub any_installed: bool,
}

/// Resolve the user-level Crew hooks directory. `KIRO_HOME` points at the Kiro
/// configuration root (the directory that contains `agents` and `crew`).
pub fn hooks_dir() -> Option<PathBuf> {
    if let Ok(kiro_home) = std::env::var("KIRO_HOME") {
        let value = kiro_home.trim();
        if !value.is_empty() {
            return Some(PathBuf::from(value).join("crew").join("hooks"));
        }
    }
    let home = dirs::home_dir().or_else(|| std::env::var("HOME").ok().map(PathBuf::from))?;
    Some(home.join(".kiro").join("crew").join("hooks"))
}

pub fn hook_file() -> Option<PathBuf> {
    hooks_dir().map(|dir| dir.join(HOOK_FILE_NAME))
}

/// Quote the executable so paths with spaces survive the platform shell.
pub fn hook_command(companion_exe: &str) -> String {
    format!("\"{companion_exe}\" {HOOK_MARKER}")
}

pub fn current_exe_path() -> String {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.to_str().map(str::to_string))
        .unwrap_or_else(|| "companion".to_string())
}

fn canonical_hook(trigger: &str, slug: &str, command: &str) -> Value {
    json!({
        "name": format!("{HOOK_NAME_PREFIX}{slug}"),
        "description": "Forward Kiro Crew lifecycle status to AK Companion",
        "trigger": trigger,
        "action": {
            "type": "command",
            "command": command,
        },
        "timeout": 5,
        "enabled": true,
    })
}

fn is_our_hook(hook: &Value) -> bool {
    hook.get("name")
        .and_then(Value::as_str)
        .map(|name| name.starts_with(HOOK_NAME_PREFIX))
        .unwrap_or(false)
}

/// Count required triggers represented by Companion-owned entries.
pub fn count_installed(config: &Value) -> u8 {
    let Some(hooks) = config.get("hooks").and_then(Value::as_array) else {
        return 0;
    };
    HOOK_TRIGGERS
        .iter()
        .filter(|(trigger, _)| {
            hooks.iter().any(|hook| {
                is_our_hook(hook)
                    && hook.get("trigger").and_then(Value::as_str) == Some(*trigger)
                    && hook
                        .pointer("/action/command")
                        .and_then(Value::as_str)
                        .map(|command| command.contains(HOOK_MARKER))
                        .unwrap_or(false)
            })
        })
        .count() as u8
}

/// Merge the four canonical AK entries into a standalone v1 hook document.
/// Non-AK entries in the same file are preserved. Existing AK entries are
/// replaced as a group, making command-path updates and duplicate cleanup
/// deterministic and idempotent.
pub fn apply_install(config: &mut Value, command: &str) -> Result<bool, String> {
    let before = config.clone();
    let root = config
        .as_object_mut()
        .ok_or("AK hook file must contain a JSON object")?;

    match root.get("version") {
        None => {
            root.insert("version".to_string(), Value::String("v1".to_string()));
        }
        Some(Value::String(version)) if version == "v1" => {}
        Some(_) => return Err("AK hook file has an unsupported schema version".to_string()),
    }

    let hooks = root
        .entry("hooks")
        .or_insert_with(|| Value::Array(Vec::new()))
        .as_array_mut()
        .ok_or("AK hook file 'hooks' must be an array")?;
    hooks.retain(|hook| !is_our_hook(hook));
    hooks.extend(
        HOOK_TRIGGERS
            .iter()
            .map(|(trigger, slug)| canonical_hook(trigger, slug, command)),
    );
    Ok(*config != before)
}

/// Remove only Companion-owned entries. Returns whether the document changed.
pub fn apply_remove(config: &mut Value) -> Result<bool, String> {
    let Some(root) = config.as_object_mut() else {
        return Err("AK hook file must contain a JSON object".to_string());
    };
    let Some(hooks) = root.get_mut("hooks") else {
        return Ok(false);
    };
    let hooks = hooks
        .as_array_mut()
        .ok_or("AK hook file 'hooks' must be an array")?;
    let before = hooks.len();
    hooks.retain(|hook| !is_our_hook(hook));
    Ok(hooks.len() != before)
}

fn read_config(path: &Path) -> Result<Option<Value>, String> {
    if !path.exists() {
        return Ok(None);
    }
    let body = std::fs::read_to_string(path)
        .map_err(|error| format!("read {}: {error}", path.display()))?;
    serde_json::from_str(&body)
        .map(Some)
        .map_err(|error| format!("parse {}: {error}", path.display()))
}

fn write_config(path: &Path, config: &Value) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("create {}: {error}", parent.display()))?;
    }
    let mut body = serde_json::to_string_pretty(config).map_err(|error| error.to_string())?;
    body.push('\n');
    std::fs::write(path, body).map_err(|error| format!("write {}: {error}", path.display()))
}

fn status_at(command: &str, dir: &Path, path: &Path) -> Result<HookStatus, String> {
    let installed_events = read_config(path)?
        .as_ref()
        .map(count_installed)
        .unwrap_or(0);
    Ok(HookStatus {
        command: command.to_string(),
        hooks_dir: dir.display().to_string(),
        hook_file: path.display().to_string(),
        installed_events,
        fully_installed: installed_events == HOOK_TRIGGERS.len() as u8,
        any_installed: installed_events > 0,
    })
}

pub fn status(command: &str) -> Result<HookStatus, String> {
    let dir = hooks_dir().ok_or("cannot resolve Kiro Crew hooks directory")?;
    let path = dir.join(HOOK_FILE_NAME);
    status_at(command, &dir, &path)
}

pub fn install(command: &str) -> Result<HookStatus, String> {
    let dir = hooks_dir().ok_or("cannot resolve Kiro Crew hooks directory")?;
    let path = dir.join(HOOK_FILE_NAME);
    let mut config = read_config(&path)?.unwrap_or_else(|| json!({ "version": "v1", "hooks": [] }));
    if apply_install(&mut config, command)? {
        write_config(&path, &config)?;
    }
    status_at(command, &dir, &path)
}

pub fn remove(command: &str) -> Result<HookStatus, String> {
    let dir = hooks_dir().ok_or("cannot resolve Kiro Crew hooks directory")?;
    let path = dir.join(HOOK_FILE_NAME);
    let Some(mut config) = read_config(&path)? else {
        return status_at(command, &dir, &path);
    };
    if apply_remove(&mut config)? {
        let empty = config
            .get("hooks")
            .and_then(Value::as_array)
            .map(Vec::is_empty)
            .unwrap_or(false);
        if empty {
            std::fs::remove_file(&path)
                .map_err(|error| format!("remove {}: {error}", path.display()))?;
        } else {
            write_config(&path, &config)?;
        }
    }
    status_at(command, &dir, &path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command() -> String {
        hook_command("C:\\Program Files\\AK Companion\\companion.exe")
    }

    #[test]
    fn hook_command_quotes_paths_with_spaces() {
        assert_eq!(
            command(),
            "\"C:\\Program Files\\AK Companion\\companion.exe\" --hook"
        );
    }

    #[test]
    fn install_creates_four_v1_global_hooks() {
        let mut config = json!({ "version": "v1", "hooks": [] });
        assert!(apply_install(&mut config, &command()).unwrap());
        assert_eq!(count_installed(&config), 4);
        assert_eq!(config["hooks"].as_array().unwrap().len(), 4);
        assert_eq!(config["hooks"][0]["trigger"], "SessionStart");
        assert_eq!(config["hooks"][3]["trigger"], "Stop");
    }

    #[test]
    fn install_is_idempotent_and_updates_command() {
        let mut config = json!({ "version": "v1", "hooks": [] });
        assert!(apply_install(&mut config, &command()).unwrap());
        assert!(!apply_install(&mut config, &command()).unwrap());
        assert!(apply_install(&mut config, "\"D:\\new.exe\" --hook").unwrap());
        assert_eq!(config["hooks"].as_array().unwrap().len(), 4);
        assert_eq!(
            config["hooks"][0]["action"]["command"],
            "\"D:\\new.exe\" --hook"
        );
    }

    #[test]
    fn install_and_remove_preserve_non_ak_entries() {
        let custom = json!({
            "name": "custom-audit",
            "trigger": "PostToolUse",
            "action": { "type": "command", "command": "echo audit" }
        });
        let mut config = json!({ "version": "v1", "hooks": [custom.clone()] });
        apply_install(&mut config, &command()).unwrap();
        assert_eq!(config["hooks"].as_array().unwrap().len(), 5);
        assert!(apply_remove(&mut config).unwrap());
        assert_eq!(config["hooks"], json!([custom]));
    }

    #[test]
    fn partial_status_counts_distinct_required_triggers() {
        let mut config = json!({ "version": "v1", "hooks": [] });
        apply_install(&mut config, &command()).unwrap();
        config["hooks"].as_array_mut().unwrap().pop();
        assert_eq!(count_installed(&config), 3);
    }

    #[test]
    fn malformed_or_unknown_schema_is_refused() {
        let mut array = json!([]);
        assert!(apply_install(&mut array, &command()).is_err());
        let mut version = json!({ "version": "v2", "hooks": [] });
        assert!(apply_install(&mut version, &command()).is_err());
        let mut hooks = json!({ "version": "v1", "hooks": {} });
        assert!(apply_install(&mut hooks, &command()).is_err());
    }
}
