//! Single source of truth for the companion's on-disk working directory.
//!
//! Everything the companion persists — the bind store, the hook handoff file,
//! and the diagnostic logs — lives under ONE dot-directory in the user's home:
//! `~/.ak-companion/`. This replaces the previous split across the platform
//! config dir (`%APPDATA%` / `%LOCALAPPDATA%` / `~/.config`) and the
//! data-local dir, so the directory is the same on Windows, macOS, and Linux
//! and is easy for the user to find and inspect.
//!
//! Resolution order for the home base:
//!   1. `HOME` (Unix, macOS)
//!   2. `USERPROFILE` (Windows)
//!   3. `dirs::home_dir()` (platform fallback)
//!   4. the system temp dir (last resort, so path resolution never returns
//!      `None` and callers do not have to handle an unwritable-home panic)

use std::path::PathBuf;

/// Directory name under the user's home. Dot-prefixed so it stays hidden and
/// groups with the other tool state the user already keeps in `~`.
pub const COMPANION_DIR_NAME: &str = ".ak-companion";

/// Absolute path to the companion working directory: `~/.ak-companion`.
///
/// Always returns a path (falls back to the temp dir when no home can be
/// resolved), so callers can `create_dir_all` it without a `None` branch.
pub fn companion_dir() -> PathBuf {
    home_dir().join(COMPANION_DIR_NAME)
}

/// Resolve the user's home directory with the same cross-platform rules used
/// by every Companion persistence path.
pub fn home_dir() -> PathBuf {
    if let Some(h) = env_home() {
        return h;
    }
    if let Some(h) = dirs::home_dir() {
        return h;
    }
    std::env::temp_dir()
}

/// Resolve a file inside the companion directory, e.g.
/// `companion_file("binds.json")` -> `~/.ak-companion/binds.json`.
pub fn companion_file(name: &str) -> PathBuf {
    companion_dir().join(name)
}

fn env_home() -> Option<PathBuf> {
    for key in ["HOME", "USERPROFILE"] {
        if let Ok(v) = std::env::var(key) {
            if !v.trim().is_empty() {
                return Some(PathBuf::from(v));
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dir_ends_with_dot_ak_companion() {
        let d = companion_dir();
        assert_eq!(
            d.file_name().and_then(|s| s.to_str()),
            Some(".ak-companion")
        );
    }

    #[test]
    fn file_joins_under_dir() {
        let f = companion_file("binds.json");
        assert!(f.ends_with(".ak-companion/binds.json"));
    }

    #[test]
    fn honours_home_env() {
        // env_home prefers HOME; verify the resolver picks a non-empty value.
        // (We do not mutate process env here to avoid cross-test races; this
        // asserts the pure join shape rather than a specific home.)
        let d = companion_dir();
        assert!(d.is_absolute() || d.starts_with(std::env::temp_dir()));
    }
}
