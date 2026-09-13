//! FP-4b Route STATUS — one-click **global hook install**.
//!
//! The status link ("the eyes") only fires when the LOCAL Kiro Crew runs the
//! `companion --hook` command on its agent lifecycle events. Wiring that by hand
//! means editing every `~/.kiro/agents/<name>.json` — this module discovers the
//! local agents and lets the UI install or remove the hook for an explicit
//! selection (including Select all).
//!
//! ## Why agent JSON and not `~/.kiro/hooks/*.sh`
//! KiroCrew's global hooks-dir autoimport only scans **`*.sh` with the exec
//! bit** — a POSIX shape. On Windows (where this companion runs) the hook is
//! executed via `%ComSpec% /c "<command>"`, so the working form is a `command`
//! string like `"C:\path\companion.exe" --hook` injected into each agent
//! config's `hooks.<event>` array. "Global" here = *every* agent file under the
//! Kiro agents dir gets the three lifecycle hooks.
//!
//! ## Idempotent + reversible
//! * `install_selected` adds one `{command}` entry to `agentSpawn` /
//!   `postToolUse` / `stop` for each selected agent, skipping any agent/event
//!   that already has a `--hook` command (so re-running never duplicates).
//! * `remove_selected` deletes the `--hook` entries only from selected agents,
//!   leaving unchecked agents and hand-authored hooks untouched. An emptied
//!   event array is removed; an emptied `hooks` object is left as `{}`.
//!
//! ## Honest limit (surfaced in the UI)
//! KiroCrew may rewrite agent specs on upgrade/rebuild, which can drop these
//! entries — so after a Kiro Crew reinstall/upgrade the user may need to click
//! "Install" again. This is a deliberate trade for a zero-config one-click.
//!
//! Cross-platform: the JSON edit + pure helpers build and unit-test on the Linux
//! CI host; only the *default companion exe path* differs per OS.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use serde::Serialize;
use serde_json::{json, Map, Value};

/// The three Kiro lifecycle events the status link wires. `agentSpawn` marks the
/// agent starting, `postToolUse` reports each tool the agent runs, `stop`
/// carries the final response — together they drive the board LCD.
pub const HOOK_EVENTS: [&str; 3] = ["agentSpawn", "postToolUse", "stop"];

/// Marker every companion-authored hook command contains, so `remove` can find
/// exactly our entries and leave hand-authored hooks alone.
pub const HOOK_MARKER: &str = "--hook";

/// Per-agent result of an install/remove/status query.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AgentHookState {
    /// Agent name (file stem, e.g. `kirocrew`).
    pub agent: String,
    /// How many of the three lifecycle events currently carry our `--hook`
    /// entry (0..=3). 3 = fully installed, 0 = absent, 1-2 = partial.
    pub installed_events: u8,
}

/// Aggregate status across all agents.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct HookStatus {
    /// The command string this companion would inject / did inject.
    pub command: String,
    /// Resolved Kiro agents directory that was scanned.
    pub agents_dir: String,
    /// Per-agent breakdown.
    pub agents: Vec<AgentHookState>,
    /// True when EVERY agent has all three events wired.
    pub fully_installed: bool,
    /// True when at least one agent has at least one event wired.
    pub any_installed: bool,
}

/// Resolve the Kiro agents directory: `<home>/.kiro/agents`. Honors `KIRO_HOME`
/// (the migration override) first, then the platform home dir.
pub fn agents_dir() -> Option<PathBuf> {
    if let Ok(kiro_home) = std::env::var("KIRO_HOME") {
        let t = kiro_home.trim();
        if !t.is_empty() {
            return Some(PathBuf::from(t).join("agents"));
        }
    }
    let home = dirs::home_dir().or_else(|| std::env::var("HOME").ok().map(PathBuf::from))?;
    Some(home.join(".kiro").join("agents"))
}

/// Build the hook command string for a given companion executable path. Quoted
/// so a path with spaces survives `%ComSpec% /c` on Windows and `/bin/sh -c` on
/// POSIX.
pub fn hook_command(companion_exe: &str) -> String {
    format!("\"{companion_exe}\" {HOOK_MARKER}")
}

/// Best-effort resolve THIS running companion's own executable path, for the
/// default command. Falls back to a placeholder the UI shows the user to edit.
pub fn current_exe_path() -> String {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.to_str().map(str::to_string))
        .unwrap_or_else(|| "companion".to_string())
}

/// Does this event's hook array already contain a companion `--hook` entry?
fn event_has_our_hook(events: Option<&Value>) -> bool {
    events
        .and_then(Value::as_array)
        .map(|arr| {
            arr.iter().any(|e| {
                e.get("command")
                    .and_then(Value::as_str)
                    .map(|c| c.contains(HOOK_MARKER))
                    .unwrap_or(false)
            })
        })
        .unwrap_or(false)
}

/// Count how many of the three lifecycle events already carry our hook, given an
/// agent config's top-level JSON. Pure — unit-tested.
pub fn count_installed(config: &Value) -> u8 {
    let hooks = config.get("hooks");
    HOOK_EVENTS
        .iter()
        .filter(|ev| event_has_our_hook(hooks.and_then(|h| h.get(**ev))))
        .count() as u8
}

/// Inject our hook into all three events of one agent config (idempotent).
/// Returns true if the config was modified. Pure — unit-tested.
pub fn apply_install(config: &mut Value, command: &str) -> bool {
    let mut changed = false;
    // Ensure `hooks` is an object.
    if !config.get("hooks").map(Value::is_object).unwrap_or(false) {
        if let Value::Object(map) = config {
            map.insert("hooks".into(), Value::Object(Map::new()));
            changed = true;
        } else {
            return false; // not a JSON object — refuse to touch.
        }
    }
    let hooks = config
        .get_mut("hooks")
        .and_then(Value::as_object_mut)
        .expect("hooks is an object");
    for ev in HOOK_EVENTS {
        let arr = hooks
            .entry(ev)
            .or_insert_with(|| Value::Array(vec![]))
            .as_array_mut();
        let Some(arr) = arr else { continue };
        let already = arr.iter().any(|e| {
            e.get("command")
                .and_then(Value::as_str)
                .map(|c| c.contains(HOOK_MARKER))
                .unwrap_or(false)
        });
        if !already {
            arr.push(json!({ "command": command }));
            changed = true;
        }
    }
    changed
}

/// Remove every companion `--hook` entry from one agent config. Returns true if
/// modified. Leaves hand-authored hooks and other events intact; drops an
/// emptied event array. Pure — unit-tested.
pub fn apply_remove(config: &mut Value) -> bool {
    let mut changed = false;
    let Some(hooks) = config.get_mut("hooks").and_then(Value::as_object_mut) else {
        return false;
    };
    for ev in HOOK_EVENTS {
        let Some(arr) = hooks.get_mut(ev).and_then(Value::as_array_mut) else {
            continue;
        };
        let before = arr.len();
        arr.retain(|e| {
            !e.get("command")
                .and_then(Value::as_str)
                .map(|c| c.contains(HOOK_MARKER))
                .unwrap_or(false)
        });
        if arr.len() != before {
            changed = true;
        }
    }
    // Drop emptied event arrays we may have just cleared.
    if changed {
        for ev in HOOK_EVENTS {
            if hooks.get(ev).and_then(Value::as_array).map(Vec::is_empty) == Some(true) {
                hooks.remove(ev);
            }
        }
    }
    changed
}

/// List `*.json` agent files in `dir`, sorted. `Err` on an unreadable dir.
fn list_agent_files(dir: &Path) -> Result<Vec<PathBuf>, String> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(dir)
        .map_err(|e| format!("read {}: {e}", dir.display()))?
        .filter_map(|entry| entry.ok().map(|e| e.path()))
        .filter(|p| p.is_file() && p.extension().map(|x| x == "json").unwrap_or(false))
        .collect();
    files.sort();
    Ok(files)
}

fn agent_name(path: &Path) -> String {
    path.file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("?")
        .to_string()
}

/// Read one agent JSON. `Err` on unreadable/unparseable so the caller can report
/// which agent failed rather than silently skipping it.
fn read_config(path: &Path) -> Result<Value, String> {
    let body =
        std::fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&body).map_err(|e| format!("parse {}: {e}", path.display()))
}

/// Write one agent JSON back, pretty-printed with a trailing newline (matches
/// how the specs are stored). Only called when the config actually changed.
fn write_config(path: &Path, config: &Value) -> Result<(), String> {
    let mut body = serde_json::to_string_pretty(config).map_err(|e| e.to_string())?;
    body.push('\n');
    std::fs::write(path, body).map_err(|e| format!("write {}: {e}", path.display()))
}

/// Scan every agent and report install status without modifying anything.
pub fn status(command: &str) -> Result<HookStatus, String> {
    let dir = agents_dir().ok_or("cannot resolve Kiro agents dir")?;
    let files = list_agent_files(&dir).unwrap_or_default();
    let mut agents = Vec::new();
    for f in &files {
        let installed_events = match read_config(f) {
            Ok(cfg) => count_installed(&cfg),
            // A file we cannot parse counts as 0 installed but is still listed.
            Err(_) => 0,
        };
        agents.push(AgentHookState {
            agent: agent_name(f),
            installed_events,
        });
    }
    let fully_installed = !agents.is_empty()
        && agents
            .iter()
            .all(|a| a.installed_events == HOOK_EVENTS.len() as u8);
    let any_installed = agents.iter().any(|a| a.installed_events > 0);
    Ok(HookStatus {
        command: command.to_string(),
        agents_dir: dir.display().to_string(),
        agents,
        fully_installed,
        any_installed,
    })
}

/// Resolve an explicit agent-name selection to files. Selection is matched only
/// against discovered file stems (never joined into a path), preventing path
/// traversal. Validate the complete selection before any write so a stale or
/// unknown name cannot cause a partial update.
fn select_agent_files(files: &[PathBuf], selected: &[String]) -> Result<Vec<PathBuf>, String> {
    if selected.is_empty() {
        return Err("select at least one agent".to_string());
    }
    let requested: HashSet<&str> = selected.iter().map(String::as_str).collect();
    let known: HashSet<String> = files.iter().map(|f| agent_name(f)).collect();
    let mut unknown: Vec<&str> = requested
        .iter()
        .copied()
        .filter(|name| !known.contains(*name))
        .collect();
    if !unknown.is_empty() {
        unknown.sort_unstable();
        return Err(format!("unknown agent(s): {}", unknown.join(", ")));
    }
    Ok(files
        .iter()
        .filter(|f| requested.contains(agent_name(f).as_str()))
        .cloned()
        .collect())
}

/// Install the `--hook` command into the selected agents (idempotent). Returns
/// the post-install status for ALL agents so the UI can keep rendering the full
/// picker. Selection is by exact agent file stem.
pub fn install_selected(command: &str, selected: &[String]) -> Result<HookStatus, String> {
    let dir = agents_dir().ok_or("cannot resolve Kiro agents dir")?;
    let files = list_agent_files(&dir)?;
    if files.is_empty() {
        return Err(format!(
            "no agent files (*.json) found in {}",
            dir.display()
        ));
    }
    for f in select_agent_files(&files, selected)? {
        let mut cfg = read_config(&f)?;
        if apply_install(&mut cfg, command) {
            write_config(&f, &cfg)?;
        }
    }
    status(command)
}

/// Remove the companion `--hook` command from the selected agents. Returns
/// post-remove status for ALL agents and leaves unselected agents untouched.
pub fn remove_selected(command: &str, selected: &[String]) -> Result<HookStatus, String> {
    let dir = agents_dir().ok_or("cannot resolve Kiro agents dir")?;
    let files = list_agent_files(&dir)?;
    for f in select_agent_files(&files, selected)? {
        let mut cfg = read_config(&f)?;
        if apply_remove(&mut cfg) {
            write_config(&f, &cfg)?;
        }
    }
    status(command)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cmd() -> String {
        hook_command("/opt/companion")
    }

    #[test]
    fn hook_command_is_quoted_with_marker() {
        assert_eq!(
            hook_command("C:\\a b\\companion.exe"),
            "\"C:\\a b\\companion.exe\" --hook"
        );
    }

    #[test]
    fn install_adds_three_events_to_empty_config() {
        let mut cfg = json!({ "name": "kirocrew" });
        assert!(apply_install(&mut cfg, &cmd()));
        assert_eq!(count_installed(&cfg), 3);
        // Each event has exactly one entry with our command.
        for ev in HOOK_EVENTS {
            let arr = cfg["hooks"][ev].as_array().unwrap();
            assert_eq!(arr.len(), 1);
            assert_eq!(arr[0]["command"], cmd());
        }
    }

    #[test]
    fn install_is_idempotent() {
        let mut cfg = json!({ "name": "a" });
        assert!(apply_install(&mut cfg, &cmd()));
        // Second application changes nothing and adds no duplicates.
        assert!(!apply_install(&mut cfg, &cmd()));
        for ev in HOOK_EVENTS {
            assert_eq!(cfg["hooks"][ev].as_array().unwrap().len(), 1);
        }
    }

    #[test]
    fn install_preserves_existing_hand_authored_hook() {
        let mut cfg = json!({
            "hooks": {
                "postToolUse": [ { "matcher": "execute_bash", "command": "echo audit" } ]
            }
        });
        assert!(apply_install(&mut cfg, &cmd()));
        // postToolUse now has the hand-authored entry PLUS ours.
        let arr = cfg["hooks"]["postToolUse"].as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert!(arr.iter().any(|e| e["command"] == "echo audit"));
        assert!(arr.iter().any(|e| e["command"] == cmd()));
    }

    #[test]
    fn remove_deletes_only_our_hook() {
        let mut cfg = json!({
            "hooks": {
                "postToolUse": [
                    { "matcher": "execute_bash", "command": "echo audit" },
                    { "command": cmd() }
                ],
                "stop": [ { "command": cmd() } ]
            }
        });
        assert!(apply_remove(&mut cfg));
        // Hand-authored audit hook survives; our postToolUse entry is gone.
        let arr = cfg["hooks"]["postToolUse"].as_array().unwrap();
        assert_eq!(arr.len(), 1);
        assert_eq!(arr[0]["command"], "echo audit");
        // stop had only our entry -> emptied array is dropped entirely.
        assert!(cfg["hooks"].get("stop").is_none());
    }

    #[test]
    fn remove_on_clean_config_is_noop() {
        let mut cfg = json!({ "hooks": { "postToolUse": [ { "command": "echo x" } ] } });
        assert!(!apply_remove(&mut cfg));
        assert_eq!(cfg["hooks"]["postToolUse"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn count_installed_partial() {
        let mut cfg = json!({ "name": "a" });
        apply_install(&mut cfg, &cmd());
        // Manually drop one event to simulate a partial install.
        cfg["hooks"].as_object_mut().unwrap().remove("stop");
        assert_eq!(count_installed(&cfg), 2);
    }

    #[test]
    fn non_object_config_is_refused() {
        let mut cfg = json!([1, 2, 3]);
        assert!(!apply_install(&mut cfg, &cmd()));
    }

    #[test]
    fn selection_targets_only_exact_agent_names() {
        let files = vec![
            PathBuf::from("/agents/kirocrew.json"),
            PathBuf::from("/agents/kirocrew-lite.json"),
            PathBuf::from("/agents/other.json"),
        ];
        let selected = vec!["kirocrew-lite".to_string(), "other".to_string()];
        assert_eq!(
            select_agent_files(&files, &selected).unwrap(),
            vec![files[1].clone(), files[2].clone()]
        );
    }

    #[test]
    fn empty_selection_is_rejected() {
        let files = vec![PathBuf::from("/agents/kirocrew.json")];
        assert_eq!(
            select_agent_files(&files, &[]).unwrap_err(),
            "select at least one agent"
        );
    }

    #[test]
    fn unknown_selection_is_rejected_before_writes() {
        let files = vec![PathBuf::from("/agents/kirocrew.json")];
        let selected = vec!["missing".to_string(), "also-missing".to_string()];
        assert_eq!(
            select_agent_files(&files, &selected).unwrap_err(),
            "unknown agent(s): also-missing, missing"
        );
    }
}
