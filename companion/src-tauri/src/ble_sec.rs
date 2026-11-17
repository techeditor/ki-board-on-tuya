//! Companion side of the BLE secure channel (`shared/protocol/protocol.md`,
//! "BLE 安全通道"). Mirrors firmware `ak_ble_sec.c` byte for byte; the
//! known-answer test below is shared with the firmware host tests.
//!
//! * pairing key K: 32 random bytes made by the keyboard, received once over
//!   USB (`ble_pair_key`), stored by [`crate::ble_pairing`];
//! * handshake: `sec_hello {key_id, nonce Nc}` -> `sec_hello_ack {nonce Nb}`;
//!   session keys = HKDF-SHA256(IKM=K, salt=Nc||Nb, info="AK-BLE-v1"),
//!   bytes 0..16 Companion->board, 16..32 board->Companion;
//! * every later line is `'~' base64(ctr[8 BE] || AES-128-CCM ct || tag[16])`,
//!   nonce = dir || 0000 || ctr, AAD = ctr; the receiver accepts only the next
//!   counter, so replay / reorder / gaps are rejected.
//!
//! All primitives are RustCrypto crates (`aes`, `ccm`, `hkdf`, `sha2`); none
//! are hand-written here.

use aes::Aes128;
use base64::Engine as _;
use ccm::aead::{Aead, KeyInit, Payload};
use ccm::consts::{U13, U16};
use ccm::Ccm;
use hkdf::Hkdf;
use sha2::{Digest, Sha256};

pub const KEY_LEN: usize = 32;
pub const NONCE_LEN: usize = 16;
const CTR_LEN: usize = 8;
const TAG_LEN: usize = 16;
const DIR_C2B: u8 = 0x01;
const DIR_B2C: u8 = 0x02;
const INFO: &[u8] = b"AK-BLE-v1";

type AesCcm = Ccm<Aes128, U16, U13>;

/// Lowercase hex of SHA-256(key)[0..4] — names a key without revealing it.
pub fn key_id(key: &[u8; KEY_LEN]) -> String {
    hex(&Sha256::digest(key)[..4])
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 || !s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return None;
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok())
        .collect()
}

/// The `sec_hello` line (with trailing newline).
pub fn hello_line(key: &[u8; KEY_LEN], nonce: &[u8; NONCE_LEN]) -> String {
    format!(
        "{{\"v\":1,\"type\":\"sec_hello\",\"payload\":{{\"key_id\":\"{}\",\"nonce\":\"{}\"}}}}\n",
        key_id(key),
        hex(nonce)
    )
}

/// Parse the board's `sec_hello_ack` and return Nb.
pub fn parse_hello_ack(line: &str) -> Option<[u8; NONCE_LEN]> {
    let v: serde_json::Value = serde_json::from_str(line.trim()).ok()?;
    if v.get("v")?.as_u64()? != 1 || v.get("type")?.as_str()? != "sec_hello_ack" {
        return None;
    }
    let nb = unhex(v.get("payload")?.get("nonce")?.as_str()?)?;
    nb.try_into().ok()
}

/// One authenticated session over one BLE connection.
pub struct Session {
    c2b: AesCcm,
    b2c: AesCcm,
    tx_ctr: u64,
    rx_ctr: u64,
}

impl Session {
    pub fn derive(key: &[u8; KEY_LEN], nc: &[u8; NONCE_LEN], nb: &[u8; NONCE_LEN]) -> Self {
        let mut salt = [0u8; 2 * NONCE_LEN];
        salt[..NONCE_LEN].copy_from_slice(nc);
        salt[NONCE_LEN..].copy_from_slice(nb);
        let mut okm = [0u8; 32];
        Hkdf::<Sha256>::new(Some(&salt), key)
            .expand(INFO, &mut okm)
            .expect("32 bytes is a valid HKDF-SHA256 length");
        let session = Self {
            c2b: AesCcm::new_from_slice(&okm[..16]).expect("16-byte AES key"),
            b2c: AesCcm::new_from_slice(&okm[16..]).expect("16-byte AES key"),
            tx_ctr: 0,
            rx_ctr: 0,
        };
        okm.fill(0);
        session
    }

    fn nonce(dir: u8, ctr: &[u8; CTR_LEN]) -> [u8; 13] {
        let mut n = [0u8; 13];
        n[0] = dir;
        n[5..].copy_from_slice(ctr);
        n
    }

    /// Seal one outgoing JSON line (trailing newline optional) into a
    /// newline-terminated sealed line.
    pub fn seal(&mut self, line: &[u8]) -> Result<Vec<u8>, String> {
        let pt = line.strip_suffix(b"\n").unwrap_or(line);
        let ctr = self.tx_ctr.to_be_bytes();
        let ct = self
            .c2b
            .encrypt(&Self::nonce(DIR_C2B, &ctr).into(), Payload { msg: pt, aad: &ctr })
            .map_err(|_| "ble seal failed".to_string())?;
        self.tx_ctr += 1;
        let mut raw = Vec::with_capacity(CTR_LEN + ct.len());
        raw.extend_from_slice(&ctr);
        raw.extend_from_slice(&ct);
        let mut out = Vec::with_capacity(raw.len() * 4 / 3 + 4);
        out.push(b'~');
        out.extend_from_slice(base64_encode(&raw).as_bytes());
        out.push(b'\n');
        Ok(out)
    }

    /// Verify and decrypt one line from the board. Any error ends the
    /// session: the caller must disconnect.
    pub fn open(&mut self, line: &str) -> Result<String, String> {
        let body = line.trim_end_matches(['\r', '\n']).strip_prefix('~').ok_or("unsealed BLE line")?;
        let raw = base64_decode(body).ok_or("bad BLE frame encoding")?;
        if raw.len() < CTR_LEN + TAG_LEN {
            return Err("short BLE frame".into());
        }
        let ctr: [u8; CTR_LEN] = raw[..CTR_LEN].try_into().expect("8 bytes");
        if u64::from_be_bytes(ctr) != self.rx_ctr {
            return Err("BLE frame counter out of order (replay?)".into());
        }
        let pt = self
            .b2c
            .decrypt(&Self::nonce(DIR_B2C, &ctr).into(), Payload { msg: &raw[CTR_LEN..], aad: &ctr })
            .map_err(|_| "BLE frame failed authentication".to_string())?;
        self.rx_ctr += 1;
        String::from_utf8(pt).map_err(|_| "BLE frame is not UTF-8".to_string())
    }
}

fn base64_encode(data: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(data)
}

fn base64_decode(s: &str) -> Option<Vec<u8>> {
    base64::engine::general_purpose::STANDARD.decode(s).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: [u8; KEY_LEN] = [0x11; KEY_LEN];

    fn nc() -> [u8; NONCE_LEN] {
        core::array::from_fn(|i| 0xA0 + i as u8)
    }
    fn nb() -> [u8; NONCE_LEN] {
        core::array::from_fn(|i| 0xB0 + i as u8)
    }

    /// Board side, for tests: seal with the board->Companion key.
    fn board_seal(key: &[u8; KEY_LEN], ctr: u64, pt: &str) -> String {
        let mut salt = [0u8; 32];
        salt[..16].copy_from_slice(&nc());
        salt[16..].copy_from_slice(&nb());
        let mut okm = [0u8; 32];
        Hkdf::<Sha256>::new(Some(&salt), key).expand(INFO, &mut okm).unwrap();
        let cipher = AesCcm::new_from_slice(&okm[16..]).unwrap();
        let c = ctr.to_be_bytes();
        let ct = cipher.encrypt(&Session::nonce(DIR_B2C, &c).into(), Payload { msg: pt.as_bytes(), aad: &c }).unwrap();
        let mut raw = c.to_vec();
        raw.extend_from_slice(&ct);
        format!("~{}\n", base64_encode(&raw))
    }

    #[test]
    fn known_answer_matches_firmware() {
        // Same vectors as firmware test_ak.c test_ble_sec.
        assert_eq!(key_id(&KEY), "02d449a3");
        let mut s = Session::derive(&KEY, &nc(), &nb());
        let line = s.seal(b"{\"v\":1,\"type\":\"hello\"}").unwrap();
        assert_eq!(
            String::from_utf8(line).unwrap(),
            "~AAAAAAAAAABIKsSEzamy/5IfkncB8vTiABm/vaxWxe3jgFyEDf19f7GeP8QS9g==\n"
        );
        let pt = s.open("~AAAAAAAAAAAPO00yq03Bk/oI8kOXUQcD/thfAhbxonLLso7p5WyqQrgb4LUy\n").unwrap();
        assert_eq!(pt, "{\"v\":1,\"type\":\"pong\"}");
    }

    #[test]
    fn hello_and_ack_lines() {
        assert_eq!(
            hello_line(&KEY, &nc()),
            "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{\"key_id\":\"02d449a3\",\"nonce\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"}}\n"
        );
        let ack = "{\"v\":1,\"type\":\"sec_hello_ack\",\"payload\":{\"nonce\":\"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf\"}}\n";
        assert_eq!(parse_hello_ack(ack), Some(nb()));
        assert_eq!(parse_hello_ack("{\"v\":1,\"type\":\"pong\"}"), None);
        assert_eq!(parse_hello_ack("{\"v\":1,\"type\":\"sec_hello_ack\",\"payload\":{\"nonce\":\"B0\"}}"), None);
    }

    #[test]
    fn counters_advance_and_replay_or_reorder_is_rejected() {
        let mut s = Session::derive(&KEY, &nc(), &nb());
        assert!(s.open(&board_seal(&KEY, 0, "{\"a\":0}")).is_ok());
        let one = board_seal(&KEY, 1, "{\"a\":1}");
        assert!(s.open(&one).is_ok());
        assert!(s.open(&one).is_err(), "replay rejected");
        let mut s = Session::derive(&KEY, &nc(), &nb());
        assert!(s.open(&board_seal(&KEY, 1, "{}")).is_err(), "out-of-order rejected");
    }

    #[test]
    fn tampering_wrong_key_and_unsealed_lines_are_rejected() {
        let good = board_seal(&KEY, 0, "{\"v\":1,\"type\":\"pong\"}");
        for at in [3usize, 20, good.len() - 4] {
            let mut bytes = good.clone().into_bytes();
            bytes[at] = if bytes[at] == b'A' { b'B' } else { b'A' };
            let mut s = Session::derive(&KEY, &nc(), &nb());
            assert!(s.open(&String::from_utf8(bytes).unwrap()).is_err(), "tamper at {at}");
        }
        let mut s = Session::derive(&[0x22; KEY_LEN], &nc(), &nb());
        assert!(s.open(&good).is_err(), "different pairing key");
        let mut s = Session::derive(&KEY, &nc(), &nb());
        assert!(s.open("{\"v\":1,\"type\":\"asr\",\"payload\":{\"text\":\"x\"}}").is_err(), "plaintext rejected");
    }

    #[test]
    fn own_frames_do_not_open_as_board_frames() {
        let mut s = Session::derive(&KEY, &nc(), &nb());
        let mine = String::from_utf8(s.seal(b"{}").unwrap()).unwrap();
        let mut peer = Session::derive(&KEY, &nc(), &nb());
        assert!(peer.open(&mine).is_err(), "direction keys differ (no reflection)");
    }

    #[test]
    fn base64_round_trips() {
        for len in 0..40 {
            let data: Vec<u8> = (0..len as u8).collect();
            assert_eq!(base64_decode(&base64_encode(&data)).unwrap(), data);
        }
        assert!(base64_decode("A=AA").is_none());
        assert!(base64_decode("AAA").is_none());
    }
}
