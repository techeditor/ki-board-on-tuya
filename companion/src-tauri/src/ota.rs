//! Local firmware update over USB (`docs/ota.md`, protocol.md "固件升级（仅 USB）").
//!
//! * [`Package::parse`] checks a `.akota` file the user picked: layout, the
//!   ECDSA P-256 signature against the public keys the firmware compiles in
//!   (read from the same C headers, so the two sides cannot drift), the image
//!   hash and the UG header. The keyboard checks everything again; this only
//!   makes a wrong file fail on the PC instead of on the device.
//! * [`Sender`] is the transfer state machine: `ota_begin`, then `ota_data`
//!   chunks with at most [`WINDOW`] chunks ahead of the last `ota_ack`, go-back
//!   on a stalled ack, `ota_end`. Pure: the app feeds it frames and a clock and
//!   writes the frames it returns — over USB only.

use base64::Engine as _;
use p256::ecdsa::signature::Verifier;
use p256::ecdsa::{Signature, VerifyingKey};
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub const MANIFEST_LEN: usize = 192;
pub const HEADER_LEN: usize = 256;
pub const CHUNK: usize = 256;
pub const WINDOW: usize = 4;
/// Resend from the last ack when nothing arrives for this long.
pub const ACK_TIMEOUT_MS: u64 = 3000;
/// After `installing`: greet every 2 s until the new firmware answers.
pub const HELLO_EVERY_MS: u64 = 2000;
pub const INSTALL_TIMEOUT_MS: u64 = 300_000;
const UG_MAGIC: u32 = 0x004D_4D4D;
const UG_TYPE_LZMA_FULL: u32 = 0x4C5A_4350;
const IMAGE_MAX: usize = 2940 * 1024 * 85 / 100;

const RELEASE_KEY_H: &str = include_str!("../../../firmware/app/common/include/ak_ota_key_release.h");
const TEST_KEY_H: &str = include_str!("../../../firmware/app/common/include/ak_ota_key_test.h");

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum KeyKind {
    Release,
    Test,
}

impl KeyKind {
    pub fn as_str(self) -> &'static str {
        match self {
            KeyKind::Release => "release",
            KeyKind::Test => "test",
        }
    }
}

/// The 65-byte uncompressed point a firmware key header compiles in, or
/// `None` for the placeholder.
pub fn header_point(header: &str) -> Option<Vec<u8>> {
    if header.contains("#define AK_OTA_KEY_PLACEHOLDER 1") {
        return None;
    }
    let start = header.find("AK_OTA_KEY_POINT")?;
    let body = &header[start..];
    let body = body[body.find('{')? + 1..body.find('}')?].replace('\\', " ");
    let point: Vec<u8> = body
        .split(',')
        .filter_map(|t| t.trim().strip_prefix("0x"))
        .filter_map(|h| u8::from_str_radix(h, 16).ok())
        .collect();
    (point.len() == 65 && point[0] == 4).then_some(point)
}

/// The keys this Companion trusts, from the firmware headers.
pub fn trusted_keys() -> Vec<(KeyKind, Vec<u8>)> {
    let mut keys = Vec::new();
    if let Some(p) = header_point(RELEASE_KEY_H) {
        keys.push((KeyKind::Release, p));
    }
    if let Some(p) = header_point(TEST_KEY_H) {
        keys.push((KeyKind::Test, p));
    }
    keys
}

fn rd16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

fn rd32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

#[derive(Clone, Debug, Serialize)]
pub struct PackageInfo {
    pub version: String,
    /// "release" or "debug" (the build profile).
    pub profile: String,
    /// Which trusted key signed it.
    pub key: KeyKind,
    pub image_size: usize,
}

#[derive(Clone, Debug)]
pub struct Package {
    pub header: Vec<u8>,
    pub image: Vec<u8>,
    pub info: PackageInfo,
}

impl Package {
    pub fn parse(bytes: &[u8], keys: &[(KeyKind, Vec<u8>)]) -> Result<Package, String> {
        if bytes.len() < HEADER_LEN + 64 {
            return Err("not a Ki-board update package (too short)".into());
        }
        let (header, image) = bytes.split_at(HEADER_LEN);
        let manifest = &header[..MANIFEST_LEN];
        if &manifest[..4] != b"AKOT" || rd16(manifest, 4) != 1 || rd16(manifest, 6) as usize != MANIFEST_LEN {
            return Err("not a Ki-board update package (.akota)".into());
        }
        if &manifest[8..16] != b"ki-board" || &manifest[24..28] != b"v0.3" {
            return Err("package is for another product or hardware".into());
        }
        let key_id = &manifest[80..88];
        let (kind, point) = keys
            .iter()
            .find(|(_, p)| &Sha256::digest(p)[..8] == key_id)
            .ok_or("package is signed by a key this Companion does not trust")?;
        let key = VerifyingKey::from_sec1_bytes(point).map_err(|_| "bad trusted key")?;
        let sig = Signature::from_slice(&header[MANIFEST_LEN..]).map_err(|_| "bad signature")?;
        key.verify(manifest, &sig).map_err(|_| "bad signature")?;
        let profile = match manifest[32] {
            1 => "release",
            2 => "debug",
            _ => return Err("unknown build profile".into()),
        };
        if *kind == KeyKind::Release && profile != "release" {
            return Err("debug build signed with the release key".into());
        }
        let size = rd32(manifest, 44) as usize;
        if size != image.len() || size > IMAGE_MAX {
            return Err("image size does not match the package".into());
        }
        if Sha256::digest(image).as_slice() != &manifest[48..80] {
            return Err("image does not match its signed hash".into());
        }
        if rd32(image, 0) != UG_MAGIC
            || rd32(image, 4) != UG_TYPE_LZMA_FULL
            || rd32(image, 32) as usize + 64 != size
            || image[..64] != manifest[128..192]
        {
            return Err("image is not a full TuyaOpen UG package".into());
        }
        let version = format!("{}.{}.{}", rd16(manifest, 36), rd16(manifest, 38), rd16(manifest, 40));
        Ok(Package {
            header: header.to_vec(),
            image: image.to_vec(),
            info: PackageInfo { version, profile: profile.into(), key: *kind, image_size: size },
        })
    }
}

/// What the UI shows.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Progress {
    /// waiting_confirm / erasing / sending / installing / installed / failed
    pub state: String,
    pub acked: usize,
    pub total: usize,
    pub reason: String,
    pub version: String,
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Phase {
    Begun,
    Erasing,
    Sending,
    Ending,
    Installing,
    Done,
}

pub struct Sender {
    pkg: Package,
    phase: Phase,
    sent: usize,
    acked: usize,
    last_ack_ms: u64,
    went_back_at: Option<usize>,
    installing_since_ms: u64,
    last_hello_ms: u64,
    failed: Option<String>,
}

fn frame(t: &str, payload: Value) -> Value {
    json!({ "v": 1, "type": t, "payload": payload })
}

impl Sender {
    /// Start a transfer: returns the sender and the `ota_begin` frame.
    pub fn start(pkg: Package, now_ms: u64) -> (Sender, Value) {
        let begin = frame(
            "ota_begin",
            json!({ "h": base64::engine::general_purpose::STANDARD.encode(&pkg.header) }),
        );
        let s = Sender { pkg, phase: Phase::Begun, sent: 0, acked: 0, last_ack_ms: now_ms, went_back_at: None,
            installing_since_ms: 0, last_hello_ms: 0, failed: None };
        (s, begin)
    }

    pub fn active(&self) -> bool {
        !matches!(self.phase, Phase::Done)
    }

    pub fn progress(&self) -> Progress {
        let state = match (self.phase, &self.failed) {
            (_, Some(_)) => "failed",
            (Phase::Begun, _) => "waiting_confirm",
            (Phase::Erasing, _) => "erasing",
            (Phase::Sending, _) | (Phase::Ending, _) => "sending",
            (Phase::Installing, _) => "installing",
            (Phase::Done, _) => "installed",
        };
        Progress {
            state: state.into(),
            acked: self.acked,
            total: self.pkg.image.len(),
            reason: self.failed.clone().unwrap_or_default(),
            version: self.pkg.info.version.clone(),
        }
    }

    fn data_frames(&mut self) -> Vec<Value> {
        let mut out = Vec::new();
        while self.sent < self.pkg.image.len() && self.sent < self.acked + WINDOW * CHUNK {
            let end = (self.sent + CHUNK).min(self.pkg.image.len());
            let d = base64::engine::general_purpose::STANDARD.encode(&self.pkg.image[self.sent..end]);
            out.push(frame("ota_data", json!({ "o": self.sent, "d": d })));
            self.sent = end;
        }
        if self.acked == self.pkg.image.len() && self.phase == Phase::Sending {
            self.phase = Phase::Ending;
            out.push(json!({ "v": 1, "type": "ota_end" }));
        }
        out
    }

    fn fail(&mut self, reason: &str) {
        self.failed = Some(reason.to_string());
        self.phase = Phase::Done;
    }

    /// An `ota_status` payload from the keyboard. Returns frames to send.
    pub fn on_status(&mut self, state: &str, reason: &str, fw: &str, now_ms: u64) -> Vec<Value> {
        match state {
            "confirm" => Vec::new(),
            "erasing" => {
                self.phase = Phase::Erasing;
                Vec::new()
            }
            "ready" if self.phase == Phase::Erasing || self.phase == Phase::Begun => {
                self.phase = Phase::Sending;
                self.sent = 0;
                self.acked = 0;
                self.last_ack_ms = now_ms;
                self.data_frames()
            }
            "installing" => {
                self.phase = Phase::Installing;
                self.installing_since_ms = now_ms;
                self.last_hello_ms = now_ms;
                Vec::new()
            }
            "failed" => {
                self.fail(if reason.is_empty() { "failed" } else { reason });
                Vec::new()
            }
            // After the reboot the new firmware answers hello with its version.
            "idle" | "installed" if self.phase == Phase::Installing => {
                if fw == self.pkg.info.version {
                    self.phase = Phase::Done;
                } else {
                    self.fail("not_installed");
                }
                Vec::new()
            }
            _ => Vec::new(),
        }
    }

    pub fn on_ack(&mut self, next: usize, now_ms: u64) -> Vec<Value> {
        if self.phase != Phase::Sending || next > self.sent {
            return Vec::new();
        }
        if next == self.acked && self.sent > next && self.went_back_at != Some(next) {
            // Duplicate ack: a line before `sent` was lost and the keyboard
            // ignores what follows it. Go back once per stall point.
            self.sent = next;
            self.went_back_at = Some(next);
        }
        if next > self.acked {
            self.acked = next;
            self.went_back_at = None;
        }
        self.last_ack_ms = now_ms;
        self.data_frames()
    }

    /// Periodic call: resend from the last ack after a silent stall; after
    /// `installing`, greet the rebooted keyboard until it reports its version.
    pub fn tick(&mut self, now_ms: u64) -> Vec<Value> {
        if self.phase == Phase::Installing {
            if now_ms.saturating_sub(self.installing_since_ms) >= INSTALL_TIMEOUT_MS {
                self.fail("no_answer");
            } else if now_ms.saturating_sub(self.last_hello_ms) >= HELLO_EVERY_MS {
                self.last_hello_ms = now_ms;
                return vec![crate::protocol::hello_payload()];
            }
            return Vec::new();
        }
        if self.phase == Phase::Sending && now_ms.saturating_sub(self.last_ack_ms) >= ACK_TIMEOUT_MS {
            self.sent = self.acked;
            self.last_ack_ms = now_ms;
            return self.data_frames();
        }
        Vec::new()
    }

    /// The user cancelled on the PC.
    pub fn abort(&mut self) -> Option<Value> {
        if matches!(self.phase, Phase::Installing | Phase::Done) {
            return None;
        }
        self.fail("aborted");
        Some(json!({ "v": 1, "type": "ota_abort" }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../firmware/app/test/ota_vectors.h");

    fn vec_bytes(name: &str) -> Vec<u8> {
        let start = VECTORS.find(&format!("{name}[")).unwrap();
        let body = &VECTORS[start..];
        let body = &body[body.find('{').unwrap() + 1..body.find('}').unwrap()];
        body.split(',')
            .filter_map(|t| t.trim().strip_prefix("0x"))
            .map(|h| u8::from_str_radix(h, 16).unwrap())
            .collect()
    }

    fn keys(kind: KeyKind) -> Vec<(KeyKind, Vec<u8>)> {
        vec![(kind, vec_bytes("ota_vec_point"))]
    }

    fn package(header: &str) -> Vec<u8> {
        let mut pkg = vec_bytes(header);
        pkg.extend(vec_bytes("ota_vec_image"));
        pkg
    }

    #[test]
    fn firmware_headers_parse() {
        assert!(header_point(RELEASE_KEY_H).is_none(), "release key is still the placeholder");
        assert_eq!(header_point(TEST_KEY_H).map(|p| p.len()), Some(65));
    }

    #[test]
    fn verifies_like_the_firmware() {
        let pkg = Package::parse(&package("ota_vec_header_release"), &keys(KeyKind::Release)).unwrap();
        assert_eq!(pkg.info.version, "0.3.60");
        assert_eq!(pkg.info.profile, "release");
        assert!(Package::parse(&package("ota_vec_header_foreign"), &keys(KeyKind::Release))
            .unwrap_err()
            .contains("does not trust"));
        assert!(Package::parse(&package("ota_vec_header_debug"), &keys(KeyKind::Release))
            .unwrap_err()
            .contains("debug build"));
        assert_eq!(
            Package::parse(&package("ota_vec_header_debug"), &keys(KeyKind::Test)).unwrap().info.profile,
            "debug"
        );
        let mut bad = package("ota_vec_header_release");
        bad[40] ^= 1;
        assert_eq!(Package::parse(&bad, &keys(KeyKind::Release)).unwrap_err(), "bad signature");
        let mut bad = package("ota_vec_header_release");
        *bad.last_mut().unwrap() ^= 1;
        assert!(Package::parse(&bad, &keys(KeyKind::Release)).unwrap_err().contains("hash"));
        assert!(Package::parse(b"short", &keys(KeyKind::Release)).is_err());
    }

    fn types(frames: &[Value]) -> Vec<String> {
        frames.iter().map(|f| f["type"].as_str().unwrap().to_string()).collect()
    }

    #[test]
    fn transfer_window_and_resend() {
        let pkg = Package::parse(&package("ota_vec_header_release"), &keys(KeyKind::Release)).unwrap();
        let total = pkg.image.len(); // 764 bytes = 3 chunks
        let (mut s, begin) = Sender::start(pkg, 0);
        assert_eq!(begin["type"], "ota_begin");
        assert_eq!(s.progress().state, "waiting_confirm");
        assert!(s.on_status("confirm", "", "0.3.54", 0).is_empty());
        assert!(s.on_status("erasing", "", "0.3.54", 0).is_empty());
        let first = s.on_status("ready", "", "0.3.54", 10);
        assert_eq!(types(&first), ["ota_data", "ota_data", "ota_data"]);
        assert_eq!(first[1]["payload"]["o"], 256);
        // Line 0 lost: the keyboard acks 0 for line 1 -> go back to 0.
        let again = s.on_ack(0, 20);
        assert_eq!(again[0]["payload"]["o"], 0);
        assert_eq!(types(&again).len(), 3);
        assert!(s.on_ack(256, 30).is_empty());
        assert!(s.on_ack(512, 40).is_empty());
        let end = s.on_ack(total, 50);
        assert_eq!(types(&end), ["ota_end"]);
        assert!(s.on_status("installing", "", "0.3.54", 60).is_empty());
        assert_eq!(s.progress().state, "installing");
        assert!(s.abort().is_none(), "no abort once the device installs");
        assert!(s.tick(1000).is_empty());
        assert_eq!(types(&s.tick(60 + HELLO_EVERY_MS)), ["hello"]);
        s.on_status("idle", "", "0.3.60", 9000);
        assert_eq!(s.progress().state, "installed");
        assert!(!s.active());
    }

    #[test]
    fn stall_failure_and_abort() {
        let pkg = Package::parse(&package("ota_vec_header_release"), &keys(KeyKind::Release)).unwrap();
        let (mut s, _) = Sender::start(pkg.clone(), 0);
        s.on_status("ready", "", "0.3.54", 0);
        assert!(s.tick(ACK_TIMEOUT_MS - 1).is_empty());
        let resent = s.tick(ACK_TIMEOUT_MS);
        assert_eq!(resent[0]["payload"]["o"], 0, "silent stall resends from the ack");
        s.on_status("failed", "hash", "0.3.54", 10);
        assert_eq!(s.progress().state, "failed");
        assert_eq!(s.progress().reason, "hash");

        let (mut s, _) = Sender::start(pkg.clone(), 0);
        assert_eq!(s.abort().unwrap()["type"], "ota_abort");
        assert_eq!(s.progress().reason, "aborted");

        let (mut s, _) = Sender::start(pkg, 0);
        s.on_status("ready", "", "0.3.54", 0);
        s.on_status("installing", "", "0.3.54", 0);
        s.on_status("idle", "", "0.3.54", 100);
        assert_eq!(s.progress().reason, "not_installed", "came back on the old version");

        let (mut s, _) = Sender::start(Package::parse(&package("ota_vec_header_release"), &keys(KeyKind::Release)).unwrap(), 0);
        s.on_status("installing", "", "0.3.54", 0);
        s.tick(INSTALL_TIMEOUT_MS);
        assert_eq!(s.progress().reason, "no_answer");
    }

    #[test]
    fn decodes_ota_frames() {
        use crate::protocol::{decode_line, Inbound};
        match decode_line(r#"{"v":1,"type":"ota_ack","payload":{"next":512}}"#) {
            Some(Inbound::OtaAck(a)) => assert_eq!(a.next, 512),
            other => panic!("{other:?}"),
        }
        match decode_line(r#"{"v":1,"type":"ota_status","payload":{"state":"idle","fw":"0.3.54","key":"test"}}"#) {
            Some(Inbound::OtaStatus(p)) => assert_eq!((p.state.as_str(), p.key.as_str()), ("idle", "test")),
            other => panic!("{other:?}"),
        }
    }
}
