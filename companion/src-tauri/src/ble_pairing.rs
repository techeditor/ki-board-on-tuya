//! Storage for the BLE pairing key received from the keyboard over USB.
//!
//! One file `ble_pairing.json` in the companion directory holding the key id
//! and the key. On Windows the key is wrapped with DPAPI
//! (`CryptProtectData`, current-user scope) so it is unreadable to other
//! accounts and off this machine. Elsewhere the key is hex in a 0600 file.
//! The key never goes to logs, diagnostics or the UI; only `key_id` does.

use crate::app_dirs;
use crate::ble_sec::{self, KEY_LEN};
use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

const FILE: &str = "ble_pairing.json";

#[derive(Serialize, Deserialize)]
struct Stored {
    key_id: String,
    /// "dpapi" (hex of the DPAPI blob) or "plain" (hex key, 0600 file).
    scheme: String,
    data: String,
}

/// What the UI may know about the pairing.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct PairingStatus {
    pub paired: bool,
    pub key_id: String,
}

fn path() -> PathBuf {
    app_dirs::companion_file(FILE)
}

/// Save a key received in a `ble_pair_key` frame. Rejects a key whose id
/// does not match (a corrupted frame).
pub fn save(key_hex: &str, key_id: &str) -> Result<PairingStatus, String> {
    save_at(&path(), key_hex, key_id)
}

pub fn load() -> Option<[u8; KEY_LEN]> {
    load_at(&path())
}

pub fn status() -> PairingStatus {
    status_at(&path())
}

pub fn forget() -> Result<(), String> {
    match fs::remove_file(path()) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("remove BLE pairing failed: {e}")),
    }
}

fn parse_key(key_hex: &str) -> Option<[u8; KEY_LEN]> {
    ble_sec::unhex(key_hex)?.try_into().ok()
}

fn save_at(path: &Path, key_hex: &str, key_id: &str) -> Result<PairingStatus, String> {
    let key = parse_key(key_hex).ok_or("ble_pair_key: malformed key")?;
    if ble_sec::key_id(&key) != key_id {
        return Err("ble_pair_key: key id mismatch".into());
    }
    let (scheme, data) = protect(&key)?;
    let stored = Stored { key_id: key_id.to_string(), scheme, data };
    let body = serde_json::to_vec(&stored).map_err(|e| e.to_string())?;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(|e| format!("create companion dir failed: {e}"))?;
    }
    write_private(path, &body).map_err(|e| format!("write BLE pairing failed: {e}"))?;
    Ok(PairingStatus { paired: true, key_id: key_id.to_string() })
}

fn load_at(path: &Path) -> Option<[u8; KEY_LEN]> {
    let stored: Stored = serde_json::from_slice(&fs::read(path).ok()?).ok()?;
    let key = unprotect(&stored.scheme, &stored.data)?;
    (ble_sec::key_id(&key) == stored.key_id).then_some(key)
}

fn status_at(path: &Path) -> PairingStatus {
    let Ok(bytes) = fs::read(path) else { return PairingStatus::default() };
    match serde_json::from_slice::<Stored>(&bytes) {
        Ok(stored) if load_at(path).is_some() => PairingStatus { paired: true, key_id: stored.key_id },
        _ => PairingStatus::default(),
    }
}

/// Create/truncate with owner-only permissions on Unix (same pattern as the
/// status-sink handoff file).
fn write_private(path: &Path, body: &[u8]) -> std::io::Result<()> {
    let mut opts = fs::OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
        opts.mode(0o600);
        if path.exists() {
            fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
        }
    }
    let mut f = opts.open(path)?;
    f.write_all(body)
}

#[cfg(not(windows))]
fn protect(key: &[u8; KEY_LEN]) -> Result<(String, String), String> {
    Ok(("plain".into(), ble_sec::hex(key)))
}

#[cfg(not(windows))]
fn unprotect(scheme: &str, data: &str) -> Option<[u8; KEY_LEN]> {
    (scheme == "plain").then(|| parse_key(data)).flatten()
}

#[cfg(windows)]
mod dpapi {
    #[repr(C)]
    pub struct Blob {
        pub cb_data: u32,
        pub pb_data: *mut u8,
    }
    #[link(name = "crypt32")]
    extern "system" {
        pub fn CryptProtectData(
            data_in: *const Blob,
            descr: *const u16,
            entropy: *const Blob,
            reserved: *const core::ffi::c_void,
            prompt: *const core::ffi::c_void,
            flags: u32,
            data_out: *mut Blob,
        ) -> i32;
        pub fn CryptUnprotectData(
            data_in: *const Blob,
            descr: *mut *mut u16,
            entropy: *const Blob,
            reserved: *const core::ffi::c_void,
            prompt: *const core::ffi::c_void,
            flags: u32,
            data_out: *mut Blob,
        ) -> i32;
    }
    #[link(name = "kernel32")]
    extern "system" {
        pub fn LocalFree(mem: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    }
    pub const CRYPTPROTECT_UI_FORBIDDEN: u32 = 0x1;

    /// Run one DPAPI call over `input`, returning the output bytes.
    pub fn call(input: &[u8], protect: bool) -> Option<Vec<u8>> {
        let inb = Blob { cb_data: input.len() as u32, pb_data: input.as_ptr() as *mut u8 };
        let mut out = Blob { cb_data: 0, pb_data: core::ptr::null_mut() };
        // SAFETY: inb points at `input` for its length; out is written by the
        // OS and freed with LocalFree below.
        let ok = unsafe {
            if protect {
                CryptProtectData(&inb, core::ptr::null(), core::ptr::null(), core::ptr::null(), core::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out)
            } else {
                CryptUnprotectData(&inb, core::ptr::null_mut(), core::ptr::null(), core::ptr::null(), core::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out)
            }
        };
        if ok == 0 || out.pb_data.is_null() {
            return None;
        }
        // SAFETY: DPAPI returned a buffer of cb_data bytes.
        let bytes = unsafe { core::slice::from_raw_parts(out.pb_data, out.cb_data as usize) }.to_vec();
        // SAFETY: the buffer was allocated by DPAPI with LocalAlloc.
        unsafe { LocalFree(out.pb_data as *mut core::ffi::c_void) };
        Some(bytes)
    }
}

#[cfg(windows)]
fn protect(key: &[u8; KEY_LEN]) -> Result<(String, String), String> {
    let blob = dpapi::call(key, true).ok_or("DPAPI CryptProtectData failed")?;
    Ok(("dpapi".into(), ble_sec::hex(&blob)))
}

#[cfg(windows)]
fn unprotect(scheme: &str, data: &str) -> Option<[u8; KEY_LEN]> {
    if scheme != "dpapi" {
        return None;
    }
    dpapi::call(&ble_sec::unhex(data)?, false)?.try_into().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ak-ble-pairing-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir.join(FILE)
    }

    #[test]
    fn save_load_status_round_trip() {
        let p = tmp("rt");
        let key = [0x11u8; KEY_LEN];
        let st = save_at(&p, &ble_sec::hex(&key), &ble_sec::key_id(&key)).unwrap();
        assert_eq!(st, PairingStatus { paired: true, key_id: "02d449a3".into() });
        assert_eq!(load_at(&p), Some(key));
        assert_eq!(status_at(&p), st);
        let _ = fs::remove_dir_all(p.parent().unwrap());
    }

    #[test]
    fn rejects_bad_frames_and_reports_unpaired() {
        let p = tmp("bad");
        assert!(save_at(&p, "zz", "00000000").is_err());
        assert!(save_at(&p, &ble_sec::hex(&[0x11; KEY_LEN]), "00000000").is_err(), "id mismatch");
        assert_eq!(status_at(&p), PairingStatus::default());
        assert_eq!(load_at(&p), None);
    }

    #[cfg(unix)]
    #[test]
    fn file_is_owner_only() {
        use std::os::unix::fs::PermissionsExt;
        let p = tmp("perm");
        let key = [0x33u8; KEY_LEN];
        save_at(&p, &ble_sec::hex(&key), &ble_sec::key_id(&key)).unwrap();
        assert_eq!(fs::metadata(&p).unwrap().permissions().mode() & 0o777, 0o600);
        let _ = fs::remove_dir_all(p.parent().unwrap());
    }

    #[test]
    fn status_never_carries_the_key() {
        let p = tmp("leak");
        let key = [0x44u8; KEY_LEN];
        let st = save_at(&p, &ble_sec::hex(&key), &ble_sec::key_id(&key)).unwrap();
        let json = serde_json::to_string(&st).unwrap();
        assert!(!json.contains(&ble_sec::hex(&key)));
        let _ = fs::remove_dir_all(p.parent().unwrap());
    }
}
