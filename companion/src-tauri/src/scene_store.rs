//! FR-03: every scene lives in `~/.ak-companion/scenes.json` (the Companion is
//! the authority for scene content; the keyboard keeps up to 5 copies).
//! Presets are built in (`scene::presets`) and never written to the file.

use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::app_dirs::companion_file;
use crate::scene::{self, Scene};

pub const STORE_FILE: &str = "scenes.json";
const STORE_VERSION: u32 = 1;
/// Keyboard slots 1..=4 hold the favourites.
pub const MAX_FAVORITES: usize = 4;

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SceneStore {
    #[serde(default)]
    pub version: u32,
    /// User scenes (presets are not stored).
    #[serde(default)]
    pub scenes: Vec<Scene>,
    /// Up to 4 scene ids, in keyboard slot order 1..=4.
    #[serde(default)]
    pub favorites: Vec<String>,
    /// Id of the scene last applied.
    #[serde(default)]
    pub current: Option<String>,
}

fn store_path() -> PathBuf {
    companion_file(STORE_FILE)
}

impl SceneStore {
    /// Load from disk. A missing file is a fresh store; a damaged file is NOT
    /// silently replaced (that would lose the user's scenes on the next save):
    /// the error is returned and the caller keeps the file.
    pub fn load() -> Result<Self, String> {
        Self::load_from(&store_path())
    }

    pub fn load_from(path: &Path) -> Result<Self, String> {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Self::fresh()),
            Err(e) => return Err(format!("read {}: {e}", path.display())),
        };
        let mut store: SceneStore =
            serde_json::from_str(&text).map_err(|e| format!("{} is damaged: {e}", path.display()))?;
        // Keep only what still validates, so a hand edit cannot push a bad
        // scene to the keyboard.
        store.scenes.retain(|s| scene::id_ok(&s.id) && !scene::is_preset_id(&s.id) && scene::validate(s).is_ok());
        store.scenes.truncate(scene::MAX_STORE_SCENES);
        let ids: Vec<String> = store.all().into_iter().map(|s| s.id).collect();
        store.favorites.retain(|f| ids.contains(f));
        store.favorites.truncate(MAX_FAVORITES);
        if store.current.as_ref().is_some_and(|c| !ids.contains(c)) {
            store.current = None;
        }
        store.version = STORE_VERSION;
        Ok(store)
    }

    /// First run: "编程" (the default keymap) is favourite 1.
    fn fresh() -> Self {
        SceneStore { version: STORE_VERSION, favorites: vec!["preset01".into()], ..Default::default() }
    }

    pub fn save(&self) -> Result<(), String> {
        self.save_to(&store_path())
    }

    /// Write a temp file next to the target, then rename it over the target,
    /// so a crash never leaves a half-written store.
    pub fn save_to(&self, path: &Path) -> Result<(), String> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("create {}: {e}", parent.display()))?;
        }
        let body = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        let tmp = path.with_extension("json.tmp");
        std::fs::write(&tmp, body).map_err(|e| format!("write {}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("replace {}: {e}", path.display()))
    }

    /// Presets first, then user scenes.
    pub fn all(&self) -> Vec<Scene> {
        let mut v = scene::presets();
        v.extend(self.scenes.iter().cloned());
        v
    }

    pub fn get(&self, id: &str) -> Option<Scene> {
        self.all().into_iter().find(|s| s.id == id)
    }

    fn new_id(&self) -> String {
        let taken: Vec<String> = self.all().into_iter().map(|s| s.id).collect();
        loop {
            let mut h = RandomState::new().build_hasher();
            h.write_u128(std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos()));
            let mut n = h.finish();
            let id: String = (0..scene::ID_LEN)
                .map(|_| {
                    let c = b"abcdefghijklmnopqrstuvwxyz0123456789"[(n % 36) as usize];
                    n /= 36;
                    c as char
                })
                .collect();
            if !taken.contains(&id) {
                return id;
            }
        }
    }

    /// `name`, or `name (2)`, `name (3)`... not used by another scene, cut to
    /// fit the byte limit at a character boundary.
    fn unique_name(&self, name: &str, skip_id: Option<&str>) -> String {
        let used: Vec<String> =
            self.all().into_iter().filter(|s| Some(s.id.as_str()) != skip_id).map(|s| s.name).collect();
        for n in 1.. {
            let suffix = if n == 1 { String::new() } else { format!(" ({n})") };
            let mut base = name.to_string();
            while base.len() + suffix.len() > scene::NAME_MAX_BYTES {
                base.pop();
            }
            let cand = format!("{}{suffix}", base.trim_end());
            if !used.contains(&cand) {
                return cand;
            }
        }
        unreachable!()
    }

    /// Add a new scene (new id, rev 0, unique name). Returns its id.
    pub fn add(&mut self, mut s: Scene) -> Result<String, String> {
        if self.scenes.len() >= scene::MAX_STORE_SCENES {
            return Err(format!("at most {} scenes", scene::MAX_STORE_SCENES));
        }
        scene::validate(&s)?;
        s.name = self.unique_name(s.name.trim(), None);
        s.id = self.new_id();
        s.rev = 0;
        let id = s.id.clone();
        self.scenes.push(s);
        Ok(id)
    }

    /// Copy any scene (presets included) as a new editable scene.
    pub fn duplicate(&mut self, id: &str) -> Result<String, String> {
        let s = self.get(id).ok_or("no such scene")?;
        self.add(s)
    }

    /// Replace a user scene's content, keeping its id; rev + 1.
    pub fn update(&mut self, id: &str, mut s: Scene) -> Result<(), String> {
        scene::validate(&s)?;
        let name = self.unique_name(s.name.trim(), Some(id));
        let cur = self.scenes.iter_mut().find(|x| x.id == id).ok_or("presets are read-only; copy it first")?;
        s.id = cur.id.clone();
        s.rev = cur.rev.wrapping_add(1);
        s.name = name;
        *cur = s;
        Ok(())
    }

    pub fn rename(&mut self, id: &str, name: &str) -> Result<(), String> {
        let mut s = self.scenes.iter().find(|x| x.id == id).cloned().ok_or("presets are read-only; copy it first")?;
        s.name = scene::check_name(name)?;
        self.update(id, s)
    }

    /// Delete a user scene. The current scene must be switched away first.
    pub fn delete(&mut self, id: &str) -> Result<(), String> {
        if self.current.as_deref() == Some(id) {
            return Err("switch to another scene before deleting this one".into());
        }
        if scene::is_preset_id(id) && !scene::PRESETS_DELETABLE {
            return Err("presets cannot be deleted".into());
        }
        let before = self.scenes.len();
        self.scenes.retain(|s| s.id != id);
        if self.scenes.len() == before {
            return Err("no such scene".into());
        }
        self.favorites.retain(|f| f != id);
        Ok(())
    }

    pub fn set_favorites(&mut self, ids: &[String]) -> Result<(), String> {
        if ids.len() > MAX_FAVORITES {
            return Err(format!("at most {MAX_FAVORITES} favourites"));
        }
        for (i, id) in ids.iter().enumerate() {
            if self.get(id).is_none() {
                return Err(format!("no such scene '{id}'"));
            }
            if ids[..i].contains(id) {
                return Err("a scene is listed twice".into());
            }
        }
        self.favorites = ids.to_vec();
        Ok(())
    }

    /// Validate an import file and add all its scenes (all or nothing).
    /// Returns the new ids.
    pub fn import(&mut self, bytes: &[u8]) -> Result<Vec<String>, String> {
        let scenes = scene::parse_file(bytes)?;
        if self.scenes.len() + scenes.len() > scene::MAX_STORE_SCENES {
            return Err(format!("at most {} scenes in total", scene::MAX_STORE_SCENES));
        }
        let mut next = self.clone();
        let ids = scenes.into_iter().map(|s| next.add(s)).collect::<Result<Vec<_>, _>>()?;
        *self = next;
        Ok(ids)
    }

    /// Export the given scenes (presets allowed).
    pub fn export(&self, ids: &[String]) -> Result<String, String> {
        let scenes = ids.iter().map(|id| self.get(id).ok_or(format!("no such scene '{id}'"))).collect::<Result<Vec<_>, _>>()?;
        if scenes.is_empty() {
            return Err("nothing to export".into());
        }
        Ok(scene::export_file(&scenes))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp_dir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("ak-scene-store-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn preset() -> Scene {
        scene::presets().remove(0)
    }

    #[test]
    fn add_update_rename_delete() {
        let mut st = SceneStore::fresh();
        let a = st.add(preset()).unwrap();
        assert!(scene::id_ok(&a));
        assert_eq!(st.get(&a).unwrap().name, "编程 (2)", "unique name vs the preset");
        let b = st.duplicate(&a).unwrap();
        assert_eq!(st.get(&b).unwrap().name, "编程 (2) (2)");
        st.rename(&a, "  Review ").unwrap();
        let s = st.get(&a).unwrap();
        assert_eq!((s.name.as_str(), s.rev), ("Review", 1));
        assert!(st.rename(&a, "").is_err());
        assert!(st.update("preset01", preset()).is_err(), "presets are read-only");
        assert!(st.delete("preset01").is_err());
        st.set_favorites(&[a.clone(), "preset01".into()]).unwrap();
        assert!(st.set_favorites(&[a.clone(), a.clone()]).is_err());
        assert!(st.set_favorites(&["nope0000".into()]).is_err());
        st.current = Some(a.clone());
        assert!(st.delete(&a).is_err(), "current scene");
        st.current = Some(b.clone());
        st.delete(&a).unwrap();
        assert_eq!(st.favorites, vec!["preset01".to_string()]);
        let mut bad = preset();
        bad.keys.k1.single = Some("evil".into());
        assert!(st.add(bad).is_err());
    }

    #[test]
    fn long_name_suffix_stays_in_limit() {
        let mut st = SceneStore::fresh();
        let mut s = preset();
        s.name = "鹏鹏鹏鹏鹏鹏鹏鹏".into(); // 24 bytes
        st.add(s.clone()).unwrap();
        let id = st.add(s).unwrap();
        let name = st.get(&id).unwrap().name;
        assert!(name.len() <= scene::NAME_MAX_BYTES && name.ends_with(" (2)"), "{name}");
    }

    #[test]
    fn import_is_all_or_nothing() {
        let mut st = SceneStore::fresh();
        let good = serde_json::to_value(preset()).unwrap();
        let mut bad = good.clone();
        bad["knob"] = serde_json::json!("spin");
        let file = serde_json::json!({"format": scene::FILE_FORMAT, "version": 1, "scenes": [good, bad]}).to_string();
        assert!(st.import(file.as_bytes()).is_err());
        assert!(st.scenes.is_empty());
        let text = st.export(&["preset01".into()]).unwrap();
        let ids = st.import(text.as_bytes()).unwrap();
        assert_eq!(ids.len(), 1);
        assert_ne!(ids[0], "preset01");
        assert_eq!(st.get(&ids[0]).unwrap().name, "编程 (2)");
    }

    #[test]
    fn save_load_round_trip_and_damage() {
        let dir = tmp_dir("rt");
        let path = dir.join(STORE_FILE);
        assert_eq!(SceneStore::load_from(&path).unwrap(), SceneStore::fresh());
        let mut st = SceneStore::fresh();
        let a = st.add(preset()).unwrap();
        st.set_favorites(&[a.clone()]).unwrap();
        st.current = Some(a);
        st.save_to(&path).unwrap();
        assert!(!dir.join("scenes.json.tmp").exists());
        assert_eq!(SceneStore::load_from(&path).unwrap(), st);

        // A hand-edited bad scene is dropped with its favourite, not pushed.
        let mut v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        v["scenes"][0]["keys"]["k1"]["single"] = serde_json::json!("evil");
        std::fs::write(&path, v.to_string()).unwrap();
        let loaded = SceneStore::load_from(&path).unwrap();
        assert!(loaded.scenes.is_empty() && loaded.favorites.is_empty() && loaded.current.is_none());

        std::fs::write(&path, "{not json").unwrap();
        assert!(SceneStore::load_from(&path).unwrap_err().contains("damaged"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
