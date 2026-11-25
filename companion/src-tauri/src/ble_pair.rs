//! Over-the-air BLE pairing (no USB, no Tuya cloud), the Companion side of
//! firmware `ak_ble_pair.c`. Protocol: `shared/protocol/protocol.md`,
//! "蓝牙直接配对". It follows BLE LE Secure Connections numeric comparison in
//! the application layer:
//!
//! * C->B `sec_pair_req {pc, cc}`, cc = SHA-256("AKPAIR-C" || Pc || Nc);
//! * B->C `sec_pair_rsp {pb, cb}`, cb = SHA-256("AKPAIR-B" || Pb || Pc || Nb);
//! * C->B `sec_pair_nonce {nc}`; B->C `sec_pair_nonce {nb}`; we check cb;
//! * both show code = BE32(SHA-256("AKPAIR-V" || Pc || Pb || Nc || Nb)[0..4]) mod 10^6;
//! * the user confirms here (`sec_pair_confirm`) and with K6 on the keyboard;
//!   the keyboard answers `sec_pair_done {key_id}`;
//! * K = HKDF-SHA256(salt = Nc || Nb, IKM = ECDH x, info = "AK-BLE-pair-v1").
//!
//! K is the same pairing key USB `ble_pair` delivers; it is stored by
//! [`crate::ble_pairing`] and used by [`crate::ble_sec`]. RustCrypto only
//! (`p256`, `sha2`, `hkdf`). The test vectors are shared with the firmware.

use crate::ble_sec::{self, KEY_LEN, NONCE_LEN};
use hkdf::Hkdf;
use p256::elliptic_curve::sec1::ToEncodedPoint;
use p256::{PublicKey, SecretKey};
use sha2::{Digest, Sha256};

pub const PUB_LEN: usize = 65;
const PRIV_LEN: usize = 32;
const CODE_MOD: u32 = 1_000_000;
const INFO: &[u8] = b"AK-BLE-pair-v1";

/// Uncompressed public key of `priv`.
pub fn public_of(priv_key: &[u8; PRIV_LEN]) -> Result<[u8; PUB_LEN], String> {
    let secret = SecretKey::from_slice(priv_key).map_err(|_| "invalid P-256 private key".to_string())?;
    let point = secret.public_key().to_encoded_point(false);
    point.as_bytes().try_into().map_err(|_| "unexpected public key length".to_string())
}

/// ECDH x coordinate. A peer key that is not an uncompressed point on the curve fails.
pub fn ecdh(priv_key: &[u8; PRIV_LEN], peer: &[u8]) -> Result<[u8; 32], String> {
    if peer.len() != PUB_LEN || peer[0] != 0x04 {
        return Err("keyboard key is not an uncompressed P-256 point".into());
    }
    let secret = SecretKey::from_slice(priv_key).map_err(|_| "invalid P-256 private key".to_string())?;
    let peer = PublicKey::from_sec1_bytes(peer).map_err(|_| "keyboard key is not on the curve".to_string())?;
    let shared = p256::ecdh::diffie_hellman(secret.to_nonzero_scalar(), peer.as_affine());
    let mut out = [0u8; 32];
    out.copy_from_slice(shared.raw_secret_bytes());
    Ok(out)
}

fn sha256(parts: &[&[u8]]) -> [u8; 32] {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

pub fn commit_c(pc: &[u8], nc: &[u8]) -> [u8; 32] {
    sha256(&[b"AKPAIR-C", pc, nc])
}

pub fn commit_b(pb: &[u8], pc: &[u8], nb: &[u8]) -> [u8; 32] {
    sha256(&[b"AKPAIR-B", pb, pc, nb])
}

pub fn code(pc: &[u8], pb: &[u8], nc: &[u8], nb: &[u8]) -> u32 {
    let h = sha256(&[b"AKPAIR-V", pc, pb, nc, nb]);
    u32::from_be_bytes([h[0], h[1], h[2], h[3]]) % CODE_MOD
}

/// "123 456", as the keyboard shows it.
pub fn format_code(code: u32) -> String {
    format!("{:03} {:03}", code / 1000, code % 1000)
}

pub fn derive(shared: &[u8; 32], nc: &[u8], nb: &[u8]) -> [u8; KEY_LEN] {
    let salt = [nc, nb].concat();
    let hk = Hkdf::<Sha256>::new(Some(&salt), shared);
    let mut key = [0u8; KEY_LEN];
    hk.expand(INFO, &mut key).expect("32 bytes is a valid HKDF-SHA256 length");
    key
}

/// `(type, payload)` of a protocol line, or None.
fn frame(line: &str) -> Option<(String, serde_json::Value)> {
    let v: serde_json::Value = serde_json::from_str(line.trim()).ok()?;
    if v.get("v")?.as_u64()? != 1 {
        return None;
    }
    let ty = v.get("type")?.as_str()?.to_string();
    let payload = v.get("payload").cloned().unwrap_or(serde_json::Value::Null);
    Some((ty, payload))
}

fn hex_field<const N: usize>(payload: &serde_json::Value, key: &str) -> Option<[u8; N]> {
    ble_sec::unhex(payload.get(key)?.as_str()?)?.try_into().ok()
}

/// What the keyboard said instead of the expected frame.
fn refusal(ty: &str, payload: &serde_json::Value) -> String {
    if ty == "sec_pair_fail" {
        let reason = payload.get("reason").and_then(|r| r.as_str()).unwrap_or("unknown");
        format!("keyboard ended the pairing: {reason}")
    } else {
        format!("unexpected frame during pairing: {ty}")
    }
}

/// True for the frame types of this exchange (the board sends only these
/// before a secure session exists).
pub fn is_pair_line(line: &str) -> bool {
    frame(line).is_some_and(|(ty, _)| ty.starts_with("sec_pair_"))
}

/// One pairing attempt, Companion side.
pub struct Pairing {
    priv_key: [u8; PRIV_LEN],
    pc: [u8; PUB_LEN],
    nc: [u8; NONCE_LEN],
    pb: [u8; PUB_LEN],
    cb: [u8; 32],
    shared: [u8; 32],
    key: Option<[u8; KEY_LEN]>,
}

impl Drop for Pairing {
    fn drop(&mut self) {
        // Best effort: do not leave the ephemeral secrets behind.
        self.priv_key = [0; PRIV_LEN];
        self.shared = [0; 32];
        self.key = None;
    }
}

impl Pairing {
    /// Fresh ephemeral key pair and nonce from the OS random source.
    pub fn new() -> Result<Self, String> {
        let mut nc = [0u8; NONCE_LEN];
        getrandom::getrandom(&mut nc).map_err(|_| "no OS random source".to_string())?;
        for _ in 0..8 {
            let mut priv_key = [0u8; PRIV_LEN];
            getrandom::getrandom(&mut priv_key).map_err(|_| "no OS random source".to_string())?;
            if let Ok(p) = Self::from_parts(priv_key, nc) {
                return Ok(p);
            }
        }
        Err("could not make a P-256 key".into())
    }

    /// Fixed inputs (tests and vectors).
    pub fn from_parts(priv_key: [u8; PRIV_LEN], nc: [u8; NONCE_LEN]) -> Result<Self, String> {
        let pc = public_of(&priv_key)?;
        Ok(Self { priv_key, pc, nc, pb: [0; PUB_LEN], cb: [0; 32], shared: [0; 32], key: None })
    }

    pub fn request_line(&self) -> String {
        format!(
            "{{\"v\":1,\"type\":\"sec_pair_req\",\"payload\":{{\"pc\":\"{}\",\"cc\":\"{}\"}}}}\n",
            ble_sec::hex(&self.pc),
            ble_sec::hex(&commit_c(&self.pc, &self.nc))
        )
    }

    /// Board `sec_pair_rsp` -> our `sec_pair_nonce` line.
    pub fn on_response(&mut self, line: &str) -> Result<String, String> {
        let (ty, payload) = frame(line).ok_or("unreadable frame during pairing")?;
        if ty != "sec_pair_rsp" {
            return Err(refusal(&ty, &payload));
        }
        self.pb = hex_field::<PUB_LEN>(&payload, "pb").ok_or("bad keyboard key")?;
        self.cb = hex_field::<32>(&payload, "cb").ok_or("bad keyboard commitment")?;
        self.shared = ecdh(&self.priv_key, &self.pb)?;
        Ok(format!(
            "{{\"v\":1,\"type\":\"sec_pair_nonce\",\"payload\":{{\"nc\":\"{}\"}}}}\n",
            ble_sec::hex(&self.nc)
        ))
    }

    /// Board `sec_pair_nonce` -> the 6-digit code to show. Fails when the
    /// keyboard nonce does not match its earlier commitment.
    pub fn on_board_nonce(&mut self, line: &str) -> Result<u32, String> {
        let (ty, payload) = frame(line).ok_or("unreadable frame during pairing")?;
        if ty != "sec_pair_nonce" {
            return Err(refusal(&ty, &payload));
        }
        let nb = hex_field::<NONCE_LEN>(&payload, "nb").ok_or("bad keyboard nonce")?;
        if commit_b(&self.pb, &self.pc, &nb) != self.cb {
            return Err("keyboard nonce does not match its commitment (possible attack); pairing stopped".into());
        }
        self.key = Some(derive(&self.shared, &self.nc, &nb));
        self.shared = [0; 32];
        Ok(code(&self.pc, &self.pb, &self.nc, &nb))
    }

    pub fn confirm_line() -> &'static str {
        "{\"v\":1,\"type\":\"sec_pair_confirm\"}\n"
    }

    pub fn cancel_line() -> &'static str {
        "{\"v\":1,\"type\":\"sec_pair_cancel\"}\n"
    }

    /// Board `sec_pair_done` -> (K, key_id). The key id must match ours.
    pub fn on_done(&mut self, line: &str) -> Result<([u8; KEY_LEN], String), String> {
        let (ty, payload) = frame(line).ok_or("unreadable frame during pairing")?;
        if ty != "sec_pair_done" {
            return Err(refusal(&ty, &payload));
        }
        let key = self.key.take().ok_or("pairing finished before the code step")?;
        let id = ble_sec::key_id(&key);
        if payload.get("key_id").and_then(|v| v.as_str()) != Some(id.as_str()) {
            return Err("keyboard key id does not match; pairing not saved".into());
        }
        Ok((key, id))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Shared with firmware/app/test/test_ak.c (test_ble_pair).
    const VECTORS: &str = include_str!("../../../shared/protocol/ble-pair-vectors.txt");

    fn v(name: &str) -> String {
        VECTORS
            .lines()
            .find_map(|l| l.strip_prefix(&format!("{name}=")))
            .unwrap_or_else(|| panic!("vector {name} missing"))
            .to_string()
    }

    fn vb<const N: usize>(name: &str) -> [u8; N] {
        ble_sec::unhex(&v(name)).unwrap().try_into().unwrap()
    }

    #[test]
    fn matches_the_shared_vectors() {
        let (priv_c, priv_b) = (vb::<32>("priv_c"), vb::<32>("priv_b"));
        let (nc, nb) = (vb::<16>("nc"), vb::<16>("nb"));
        let (pc, pb) = (vb::<65>("pc"), vb::<65>("pb"));
        assert_eq!(public_of(&priv_c).unwrap(), pc);
        assert_eq!(public_of(&priv_b).unwrap(), pb);
        let z = ecdh(&priv_c, &pb).unwrap();
        assert_eq!(z, vb::<32>("shared"));
        assert_eq!(ecdh(&priv_b, &pc).unwrap(), z);
        assert_eq!(commit_c(&pc, &nc), vb::<32>("cc"));
        assert_eq!(commit_b(&pb, &pc, &nb), vb::<32>("cb"));
        assert_eq!(format!("{:06}", code(&pc, &pb, &nc, &nb)), v("code"));
        let key = derive(&z, &nc, &nb);
        assert_eq!(key, vb::<32>("key"));
        assert_eq!(ble_sec::key_id(&key), v("key_id"));
    }

    fn board_rsp(priv_b: &[u8; 32], pc: &[u8], nb: &[u8]) -> String {
        let pb = public_of(priv_b).unwrap();
        format!(
            "{{\"v\":1,\"type\":\"sec_pair_rsp\",\"payload\":{{\"pb\":\"{}\",\"cb\":\"{}\"}}}}",
            ble_sec::hex(&pb),
            ble_sec::hex(&commit_b(&pb, pc, nb))
        )
    }

    fn nonce_line(nb: &[u8]) -> String {
        format!("{{\"v\":1,\"type\":\"sec_pair_nonce\",\"payload\":{{\"nb\":\"{}\"}}}}", ble_sec::hex(nb))
    }

    #[test]
    fn full_exchange_with_the_vector_inputs() {
        let (priv_b, nb) = (vb::<32>("priv_b"), vb::<16>("nb"));
        let mut p = Pairing::from_parts(vb::<32>("priv_c"), vb::<16>("nc")).unwrap();
        let req = p.request_line();
        assert!(req.contains(&v("pc")) && req.contains(&v("cc")) && is_pair_line(&req));
        let nonce = p.on_response(&board_rsp(&priv_b, &p.pc.clone(), &nb)).unwrap();
        assert!(nonce.contains(&v("nc")));
        assert_eq!(format!("{:06}", p.on_board_nonce(&nonce_line(&nb)).unwrap()), v("code"));
        let done = format!("{{\"v\":1,\"type\":\"sec_pair_done\",\"payload\":{{\"key_id\":\"{}\"}}}}", v("key_id"));
        let (key, id) = p.on_done(&done).unwrap();
        assert_eq!(key, vb::<32>("key"));
        assert_eq!(id, v("key_id"));
        assert_eq!(format_code(193_115), "193 115");
        assert_eq!(format_code(7), "000 007");
    }

    #[test]
    fn rejects_a_nonce_that_breaks_the_commitment() {
        let (priv_b, nb) = (vb::<32>("priv_b"), vb::<16>("nb"));
        let mut p = Pairing::from_parts(vb::<32>("priv_c"), vb::<16>("nc")).unwrap();
        p.on_response(&board_rsp(&priv_b, &p.pc.clone(), &nb)).unwrap();
        let mut other = nb;
        other[0] ^= 1;
        let err = p.on_board_nonce(&nonce_line(&other)).unwrap_err();
        assert!(err.contains("commitment"), "{err}");
    }

    #[test]
    fn reports_a_keyboard_refusal_and_bad_frames() {
        let mut p = Pairing::new().unwrap();
        let err = p
            .on_response("{\"v\":1,\"type\":\"sec_pair_fail\",\"payload\":{\"reason\":\"timeout\"}}")
            .unwrap_err();
        assert!(err.contains("timeout"), "{err}");
        let mut bad = vb::<65>("pb");
        bad[64] ^= 1;
        let rsp = format!(
            "{{\"v\":1,\"type\":\"sec_pair_rsp\",\"payload\":{{\"pb\":\"{}\",\"cb\":\"{}\"}}}}",
            ble_sec::hex(&bad),
            v("cb")
        );
        assert!(p.on_response(&rsp).is_err(), "a point off the curve is refused");
        assert!(!is_pair_line("{\"v\":1,\"type\":\"sec_hello_ack\",\"payload\":{}}"));
    }

    #[test]
    fn done_with_a_wrong_key_id_is_not_saved() {
        let (priv_b, nb) = (vb::<32>("priv_b"), vb::<16>("nb"));
        let mut p = Pairing::from_parts(vb::<32>("priv_c"), vb::<16>("nc")).unwrap();
        p.on_response(&board_rsp(&priv_b, &p.pc.clone(), &nb)).unwrap();
        p.on_board_nonce(&nonce_line(&nb)).unwrap();
        let done = "{\"v\":1,\"type\":\"sec_pair_done\",\"payload\":{\"key_id\":\"00000000\"}}";
        assert!(p.on_done(done).is_err());
    }

    #[test]
    fn two_fresh_attempts_differ() {
        let a = Pairing::new().unwrap();
        let b = Pairing::new().unwrap();
        assert_ne!(a.request_line(), b.request_line());
    }
}
