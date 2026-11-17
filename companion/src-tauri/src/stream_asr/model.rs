//! Download manager for the local engine: the model and the sherpa-onnx
//! runtime are fetched on demand into `~/.ak-companion/asr-local/`, every
//! file checked against a pinned sha256. Nothing here is in the installer.
//!
//! Model: sherpa-onnx streaming Paraformer, Chinese + English, int8
//! (`csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en`, converted
//! from FunASR `speech_paraformer_asr_nat-zh-cn-16k-common-vocab8404-online`).
//! Weights: FunASR Model Open Source License 1.1 (use, copy, modify, share,
//! commercial use included; keep attribution and the model name). Conversion
//! repo: Apache-2.0. Download ~237 MB, same on disk.
//!
//! Runtime: sherpa-onnx v2.0.1 Windows x64 shared libraries (Apache-2.0) with
//! onnxruntime (MIT). Download 7.9 MB, ~22 MB on disk.

use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use serde::Serialize;
use sha2::{Digest, Sha256};

use super::sherpa::LocalPaths;
use crate::app_dirs::companion_dir;

pub const MODEL_ID: &str = "paraformer-zh-en-streaming-int8";
pub const MODEL_NAME: &str = "Paraformer streaming zh-en (FunASR, int8)";
pub const MODEL_LICENSE: &str = "FunASR Model License 1.1 (commercial use allowed, attribution required)";
pub const RUNTIME_VERSION: &str = "sherpa-onnx-v2.0.1";

/// One file with its pinned hash. `urls` are tried in order (a mirror for
/// networks where the first host is slow); the hash decides, not the host.
#[derive(Clone, Copy, Debug)]
pub struct RemoteFile {
    pub name: &'static str,
    pub urls: &'static [&'static str],
    pub sha256: &'static str,
    pub size: u64,
}

/// A `.tar.bz2` of which only `files` (under `prefix`) are kept.
#[derive(Clone, Copy, Debug)]
pub struct RuntimeArchive {
    pub archive: RemoteFile,
    pub prefix: &'static str,
    pub files: &'static [&'static str],
}

// Hugging Face revision pinned so the files cannot change under the hashes.
pub const MODEL_FILES: [RemoteFile; 3] = [
    RemoteFile {
        name: "encoder.int8.onnx",
        urls: &[
            "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/encoder.int8.onnx",
            "https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/encoder.int8.onnx",
        ],
        sha256: "81a70226a8934e6ed92aa1d4fc486b428b5398e2f2619ed4897b7294cab90e9a",
        size: 165_462_184,
    },
    RemoteFile {
        name: "decoder.int8.onnx",
        urls: &[
            "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/decoder.int8.onnx",
            "https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/decoder.int8.onnx",
        ],
        sha256: "f3cca9f77bb9d93c8fcbfb63ae617b6b1ee96818df3aa3b151c40658fe38594f",
        size: 71_664_561,
    },
    RemoteFile {
        name: "tokens.txt",
        urls: &[
            "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/tokens.txt",
            "https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/8e40c43232a1c5c66c82111efc5820d3accca11b/tokens.txt",
        ],
        sha256: "59aba8873a2ed1e122c25fee421e25f283b63290efbde85c1f01a853d83cb6e6",
        size: 75_756,
    },
];

#[cfg(target_os = "windows")]
pub const RUNTIME: Option<RuntimeArchive> = Some(RuntimeArchive {
    archive: RemoteFile {
        name: "sherpa-onnx-v2.0.1-win-x64-shared-MT-Release-lib.tar.bz2",
        urls: &["https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MT-Release-lib.tar.bz2"],
        sha256: "afa4de3ca4ab0afd53bb4aab40d28ee15608364b549ffc0fe0caf6489bfc39a5",
        size: 7_899_712,
    },
    prefix: "sherpa-onnx-v2.0.1-win-x64-shared-MT-Release-lib/lib/",
    files: &["onnxruntime.dll", "onnxruntime_providers_shared.dll", "sherpa-onnx-c-api.dll"],
});
/// The app only ships on Windows; elsewhere the runtime is supplied by hand
/// (see the ignored real-model test in `sherpa`).
#[cfg(not(target_os = "windows"))]
pub const RUNTIME: Option<RuntimeArchive> = None;

pub fn root() -> PathBuf {
    companion_dir().join("asr-local")
}

pub fn model_dir() -> PathBuf {
    root().join(MODEL_ID)
}

pub fn runtime_dir() -> PathBuf {
    root().join(RUNTIME_VERSION)
}

pub fn paths() -> LocalPaths {
    let m = model_dir();
    LocalPaths {
        runtime_dir: runtime_dir(),
        encoder: m.join("encoder.int8.onnx"),
        decoder: m.join("decoder.int8.onnx"),
        tokens: m.join("tokens.txt"),
    }
}

/// Download size of everything (model + runtime).
pub fn download_bytes() -> u64 {
    MODEL_FILES.iter().map(|f| f.size).sum::<u64>() + RUNTIME.map(|r| r.archive.size).unwrap_or(0)
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct LocalStatus {
    pub installed: bool,
    pub supported: bool,
    pub download_bytes: u64,
    pub model: &'static str,
    pub license: &'static str,
    pub dir: String,
}

const MARKER: &str = "verified.json";

/// Installed = every model file has its size and the marker written after
/// the hashes were checked lists the same hashes; runtime files present.
pub fn status() -> LocalStatus {
    LocalStatus {
        installed: RUNTIME.is_some_and(|r| installed_in(&model_dir(), &runtime_dir(), &MODEL_FILES, &r)),
        supported: RUNTIME.is_some(),
        download_bytes: download_bytes(),
        model: MODEL_NAME,
        license: MODEL_LICENSE,
        dir: root().display().to_string(),
    }
}

fn marker_body(files: &[RemoteFile], runtime: &RuntimeArchive) -> String {
    let mut shas: Vec<&str> = files.iter().map(|f| f.sha256).collect();
    shas.push(runtime.archive.sha256);
    serde_json::json!({ "sha256": shas }).to_string()
}

fn installed_in(model_dir: &Path, runtime_dir: &Path, files: &[RemoteFile], runtime: &RuntimeArchive) -> bool {
    let sized = files
        .iter()
        .all(|f| fs::metadata(model_dir.join(f.name)).map(|m| m.len() == f.size).unwrap_or(false));
    let libs = runtime.files.iter().all(|f| runtime_dir.join(f).is_file());
    let marker = fs::read_to_string(model_dir.join(MARKER)).map(|m| m == marker_body(files, runtime)).unwrap_or(false);
    sized && libs && marker
}

/// Progress callback: (bytes done, bytes total).
pub type Progress<'a> = &'a mut dyn FnMut(u64, u64);

pub fn install(progress: Progress, cancel: &AtomicBool) -> Result<(), String> {
    let runtime = RUNTIME.ok_or("the local engine is only available on Windows")?;
    super::sherpa::unload();
    install_into(&model_dir(), &runtime_dir(), &MODEL_FILES, &runtime, progress, cancel)
}

/// Delete the downloaded model and runtime (frees ~260 MB).
pub fn remove() -> Result<(), String> {
    super::sherpa::unload();
    for d in [model_dir(), runtime_dir()] {
        if d.exists() {
            fs::remove_dir_all(&d).map_err(|e| format!("delete {}: {e}", d.display()))?;
        }
    }
    Ok(())
}

pub(crate) fn install_into(
    model_dir: &Path,
    runtime_dir: &Path,
    files: &[RemoteFile],
    runtime: &RuntimeArchive,
    progress: Progress,
    cancel: &AtomicBool,
) -> Result<(), String> {
    fs::create_dir_all(model_dir).map_err(|e| format!("create {}: {e}", model_dir.display()))?;
    fs::create_dir_all(runtime_dir).map_err(|e| format!("create {}: {e}", runtime_dir.display()))?;
    let _ = fs::remove_file(model_dir.join(MARKER));
    let total = files.iter().map(|f| f.size).sum::<u64>() + runtime.archive.size;
    let mut done = 0u64;
    let client = reqwest::blocking::Client::builder()
        .connect_timeout(Duration::from_secs(15))
        .timeout(None::<Duration>)
        .user_agent(concat!("ki-board-companion/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|e| format!("HTTP client: {e}"))?;

    for f in files {
        let dest = model_dir.join(f.name);
        if fs::metadata(&dest).map(|m| m.len() == f.size).unwrap_or(false) && sha256_file(&dest)? == f.sha256 {
            done += f.size;
            progress(done, total);
            continue;
        }
        let part = model_dir.join(format!("{}.part", f.name));
        let mut file = File::create(&part).map_err(|e| format!("create {}: {e}", part.display()))?;
        let r = fetch(&client, f, &mut file, done, total, progress, cancel);
        drop(file);
        if let Err(e) = r {
            let _ = fs::remove_file(&part);
            return Err(e);
        }
        fs::rename(&part, &dest).map_err(|e| format!("rename {}: {e}", dest.display()))?;
        done += f.size;
    }

    let mut archive = Vec::with_capacity(runtime.archive.size as usize);
    fetch(&client, &runtime.archive, &mut archive, done, total, progress, cancel)?;
    extract_runtime(&archive, runtime, runtime_dir)?;
    done += runtime.archive.size;
    progress(done, total);

    fs::write(model_dir.join(MARKER), marker_body(files, runtime)).map_err(|e| format!("write marker: {e}"))?;
    Ok(())
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut f = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("read {}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}

/// Try each URL until one gives the expected bytes; `out` is reset before
/// each attempt, so a bad mirror leaves nothing behind.
fn fetch(
    client: &reqwest::blocking::Client,
    file: &RemoteFile,
    out: &mut dyn WriteSeek,
    base: u64,
    total: u64,
    progress: Progress,
    cancel: &AtomicBool,
) -> Result<(), String> {
    let mut last_error = String::from("no URL");
    for url in file.urls {
        out.reset().map_err(|e| format!("reset {}: {e}", file.name))?;
        match fetch_one(client, url, file, out, base, total, progress, cancel) {
            Ok(()) => return Ok(()),
            Err(e) if cancel.load(Ordering::Acquire) => return Err(e),
            Err(e) => last_error = e,
        }
    }
    Err(last_error)
}

#[allow(clippy::too_many_arguments)]
fn fetch_one(
    client: &reqwest::blocking::Client,
    url: &str,
    file: &RemoteFile,
    out: &mut dyn WriteSeek,
    base: u64,
    total: u64,
    progress: Progress,
    cancel: &AtomicBool,
) -> Result<(), String> {
    let host = url.split('/').nth(2).unwrap_or(url).to_string();
    let mut resp = client.get(url).send().map_err(|e| format!("{}: {host}: {e}", file.name))?;
    if !resp.status().is_success() {
        return Err(format!("{}: {host}: HTTP {}", file.name, resp.status().as_u16()));
    }
    let mut h = Sha256::new();
    let mut got = 0u64;
    let mut buf = vec![0u8; 1 << 16];
    loop {
        if cancel.load(Ordering::Acquire) {
            return Err("download cancelled".into());
        }
        let n = resp.read(&mut buf).map_err(|e| format!("{}: {host}: {e}", file.name))?;
        if n == 0 {
            break;
        }
        got += n as u64;
        if got > file.size {
            return Err(format!("{}: {host}: larger than expected", file.name));
        }
        h.update(&buf[..n]);
        out.write_all(&buf[..n]).map_err(|e| format!("write {}: {e}", file.name))?;
        progress(base + got, total);
    }
    let sha = format!("{:x}", h.finalize());
    if sha != file.sha256 {
        return Err(format!("{}: {host}: sha256 mismatch", file.name));
    }
    out.flush().map_err(|e| format!("write {}: {e}", file.name))
}

/// A download target that can start over (file or memory).
trait WriteSeek: Write {
    fn reset(&mut self) -> std::io::Result<()>;
}

impl WriteSeek for File {
    fn reset(&mut self) -> std::io::Result<()> {
        use std::io::Seek;
        self.set_len(0)?;
        self.seek(std::io::SeekFrom::Start(0)).map(|_| ())
    }
}

impl WriteSeek for Vec<u8> {
    fn reset(&mut self) -> std::io::Result<()> {
        self.clear();
        Ok(())
    }
}

/// Keep only the listed libraries from the (already verified) archive.
fn extract_runtime(archive: &[u8], runtime: &RuntimeArchive, dir: &Path) -> Result<(), String> {
    let mut tar = tar::Archive::new(bzip2::read::BzDecoder::new(archive));
    let mut found = Vec::new();
    for entry in tar.entries().map_err(|e| format!("runtime archive: {e}"))? {
        let mut entry = entry.map_err(|e| format!("runtime archive: {e}"))?;
        let path = entry.path().map_err(|e| format!("runtime archive: {e}"))?.to_string_lossy().to_string();
        let Some(name) = path.strip_prefix(runtime.prefix) else { continue };
        if !runtime.files.contains(&name) {
            continue;
        }
        let part = dir.join(format!("{name}.part"));
        let mut out = File::create(&part).map_err(|e| format!("create {}: {e}", part.display()))?;
        std::io::copy(&mut entry, &mut out).map_err(|e| format!("extract {name}: {e}"))?;
        drop(out);
        fs::rename(&part, dir.join(name)).map_err(|e| format!("rename {name}: {e}"))?;
        found.push(name.to_string());
    }
    for f in runtime.files {
        if !found.iter().any(|n| n == f) {
            return Err(format!("runtime archive is missing {f}"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    fn sha(b: &[u8]) -> String {
        format!("{:x}", Sha256::digest(b))
    }

    fn leak(s: String) -> &'static str {
        Box::leak(s.into_boxed_str())
    }

    fn tar_bz2(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut builder = tar::Builder::new(Vec::new());
        for (path, body) in entries {
            let mut h = tar::Header::new_gnu();
            h.set_size(body.len() as u64);
            h.set_mode(0o644);
            h.set_cksum();
            builder.append_data(&mut h, path, *body).unwrap();
        }
        let tar = builder.into_inner().unwrap();
        let mut enc = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::fast());
        enc.write_all(&tar).unwrap();
        enc.finish().unwrap()
    }

    /// Serves `/good/<name>` from `files`, `/bad/*` as 404 and `/evil/*` as
    /// wrong bytes, until the test ends.
    fn server(files: Vec<(&'static str, Vec<u8>)>) -> String {
        let srv = Arc::new(tiny_http::Server::http("127.0.0.1:0").unwrap());
        let base = format!("http://{}", srv.server_addr().to_ip().unwrap());
        std::thread::spawn(move || {
            for req in srv.incoming_requests() {
                let url = req.url().to_string();
                let name = url.rsplit('/').next().unwrap_or("");
                let body = files.iter().find(|(n, _)| *n == name).map(|(_, b)| b.clone());
                let resp = match (url.starts_with("/good/"), url.starts_with("/evil/"), body) {
                    (true, _, Some(b)) => tiny_http::Response::from_data(b).with_status_code(200),
                    (_, true, Some(b)) => tiny_http::Response::from_data(vec![b'x'; b.len()]).with_status_code(200),
                    _ => tiny_http::Response::from_data(Vec::new()).with_status_code(404),
                };
                let _ = req.respond(resp);
            }
        });
        base
    }

    #[test]
    fn installs_verifies_extracts_and_tolerates_a_bad_mirror() {
        let model = b"model bytes".to_vec();
        let tokens = "你 1\n好 2\n".as_bytes().to_vec();
        let archive = tar_bz2(&[
            ("rt/lib/a.dll", b"AAA"),
            ("rt/lib/b.dll", b"BBBB"),
            ("rt/lib/unused.lib", b"ignored"),
        ]);
        let base = server(vec![("m.onnx", model.clone()), ("t.txt", tokens.clone()), ("rt.tar.bz2", archive.clone())]);
        let files = [
            RemoteFile {
                name: "m.onnx",
                urls: Box::leak(vec![leak(format!("{base}/evil/m.onnx")), leak(format!("{base}/good/m.onnx"))].into_boxed_slice()),
                sha256: leak(sha(&model)),
                size: model.len() as u64,
            },
            RemoteFile {
                name: "t.txt",
                urls: Box::leak(vec![leak(format!("{base}/bad/t.txt")), leak(format!("{base}/good/t.txt"))].into_boxed_slice()),
                sha256: leak(sha(&tokens)),
                size: tokens.len() as u64,
            },
        ];
        let runtime = RuntimeArchive {
            archive: RemoteFile {
                name: "rt.tar.bz2",
                urls: Box::leak(vec![leak(format!("{base}/good/rt.tar.bz2"))].into_boxed_slice()),
                sha256: leak(sha(&archive)),
                size: archive.len() as u64,
            },
            prefix: "rt/lib/",
            files: &["a.dll", "b.dll"],
        };
        let tmp = std::env::temp_dir().join(format!("ak-asr-model-{}", super::super::random_id()));
        let (mdir, rdir) = (tmp.join("model"), tmp.join("rt"));
        let mut last = (0, 0);
        let cancel = AtomicBool::new(false);
        install_into(&mdir, &rdir, &files, &runtime, &mut |d, t| last = (d, t), &cancel).unwrap();
        assert_eq!(last.0, last.1);
        assert_eq!(fs::read(mdir.join("m.onnx")).unwrap(), model);
        assert_eq!(fs::read(rdir.join("b.dll")).unwrap(), b"BBBB");
        assert!(!rdir.join("unused.lib").exists());
        assert!(!mdir.join("m.onnx.part").exists());
        assert!(installed_in(&mdir, &rdir, &files, &runtime));

        // A tampered file is no longer "installed".
        fs::write(mdir.join("t.txt"), b"short").unwrap();
        assert!(!installed_in(&mdir, &rdir, &files, &runtime));

        // Every mirror wrong -> error, nothing left behind, marker gone.
        let mut wrong = files;
        wrong[1].sha256 = "00";
        let err = install_into(&mdir, &rdir, &wrong, &runtime, &mut |_, _| {}, &cancel).unwrap_err();
        assert!(err.contains("t.txt"), "{err}");
        assert!(!mdir.join("t.txt.part").exists());
        assert!(!installed_in(&mdir, &rdir, &files, &runtime));

        // Cancel stops at once.
        cancel.store(true, Ordering::Release);
        fs::remove_file(mdir.join("m.onnx")).unwrap();
        assert!(install_into(&mdir, &rdir, &files, &runtime, &mut |_, _| {}, &cancel).unwrap_err().contains("cancelled"));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn archive_without_a_listed_library_is_refused() {
        let archive = tar_bz2(&[("rt/lib/a.dll", b"AAA")]);
        let runtime = RuntimeArchive {
            archive: RemoteFile { name: "x", urls: &[], sha256: "", size: 0 },
            prefix: "rt/lib/",
            files: &["a.dll", "b.dll"],
        };
        let tmp = std::env::temp_dir().join(format!("ak-asr-rt-{}", super::super::random_id()));
        fs::create_dir_all(&tmp).unwrap();
        assert!(extract_runtime(&archive, &runtime, &tmp).unwrap_err().contains("b.dll"));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn manifest_is_consistent() {
        for f in MODEL_FILES {
            assert_eq!(f.sha256.len(), 64);
            assert!(f.urls.iter().all(|u| u.starts_with("https://") && u.contains("8e40c43232a1c5c66c82111efc5820d3accca11b") && u.ends_with(f.name)));
        }
        assert!(download_bytes() >= 237_000_000);
        let p = paths();
        assert!(p.encoder.starts_with(model_dir()) && p.runtime_dir.ends_with(RUNTIME_VERSION));
    }
}
