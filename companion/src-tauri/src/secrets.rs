//! API keys in the system keychain only: Windows Credential Manager (and the
//! macOS Keychain through the same crate when a macOS build exists). Keys are
//! never written to a config file, never logged, and never sent back to the
//! web UI; the UI only learns whether a key is stored.

/// Credential Manager "target" prefix / Keychain service.
pub const SERVICE: &str = "Ki-board Companion";

/// One stored secret per account, e.g. `asr.doubao`.
pub trait SecretStore: Send + Sync {
    fn get(&self, account: &str) -> Result<Option<String>, String>;
    fn set(&self, account: &str, secret: &str) -> Result<(), String>;
    fn delete(&self, account: &str) -> Result<(), String>;
}

/// Trim and check a pasted key: 8..=512 printable ASCII characters, no
/// spaces. The error never echoes the key.
pub fn validate_key(raw: &str) -> Result<String, String> {
    let key = raw.trim();
    if key.len() < 8 || key.len() > 512 {
        return Err("the key must be 8 to 512 characters".into());
    }
    if !key.chars().all(|c| c.is_ascii_graphic()) {
        return Err("the key may only contain visible ASCII characters".into());
    }
    Ok(key.to_string())
}

pub fn system() -> Box<dyn SecretStore> {
    #[cfg(target_os = "windows")]
    {
        Box::new(keychain::Keychain)
    }
    #[cfg(not(target_os = "windows"))]
    {
        Box::new(Unsupported)
    }
}

#[cfg(target_os = "windows")]
mod keychain {
    use super::{SecretStore, SERVICE};

    pub struct Keychain;

    fn entry(account: &str) -> Result<keyring::Entry, String> {
        keyring::Entry::new(SERVICE, account).map_err(|e| format!("keychain: {e}"))
    }

    impl SecretStore for Keychain {
        fn get(&self, account: &str) -> Result<Option<String>, String> {
            match entry(account)?.get_password() {
                Ok(s) => Ok(Some(s)),
                Err(keyring::Error::NoEntry) => Ok(None),
                Err(e) => Err(format!("keychain read: {e}")),
            }
        }

        fn set(&self, account: &str, secret: &str) -> Result<(), String> {
            entry(account)?.set_password(secret).map_err(|e| format!("keychain write: {e}"))
        }

        fn delete(&self, account: &str) -> Result<(), String> {
            match entry(account)?.delete_credential() {
                Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
                Err(e) => Err(format!("keychain delete: {e}")),
            }
        }
    }
}

/// Hosts without a supported keychain store nothing (cloud engines then
/// report "no key"); there is deliberately no file fallback.
#[cfg(not(target_os = "windows"))]
struct Unsupported;

#[cfg(not(target_os = "windows"))]
impl SecretStore for Unsupported {
    fn get(&self, _: &str) -> Result<Option<String>, String> {
        Ok(None)
    }
    fn set(&self, _: &str, _: &str) -> Result<(), String> {
        Err("no system keychain on this host".into())
    }
    fn delete(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_are_trimmed_and_checked_without_echo() {
        assert_eq!(validate_key("  sk-abcdef123456 \n").unwrap(), "sk-abcdef123456");
        for bad in ["short", "has space inside!", "中文中文中文中文", &"x".repeat(513)] {
            let e = validate_key(bad).unwrap_err();
            assert!(!e.contains(bad.trim()), "error echoed the key: {e}");
        }
    }

    #[cfg(not(target_os = "windows"))]
    #[test]
    fn no_keychain_means_no_key_and_no_file() {
        let s = system();
        assert_eq!(s.get("asr.doubao").unwrap(), None);
        assert!(s.set("asr.doubao", "sk-12345678").is_err());
    }
}
