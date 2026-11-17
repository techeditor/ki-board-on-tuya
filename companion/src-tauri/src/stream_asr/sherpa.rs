//! Local, offline engine: sherpa-onnx streaming Paraformer (zh + en).
//!
//! The sherpa-onnx C API (`sherpa-onnx-c-api.dll` + `onnxruntime.dll`,
//! v2.0.1, Apache-2.0 / MIT) is loaded at run time from the folder the model
//! manager downloads it to (see `model`), so the installer carries neither the
//! model nor the runtime, and a missing download only disables this engine.
//!
//! The `#[repr(C)]` structs below mirror `sherpa-onnx/c-api/c-api.h` at tag
//! v2.0.1 (the same layout as the official `sherpa-onnx-sys` 2.0.1 crate).
//! The runtime version is pinned by sha256 in `model`, so the layout cannot
//! drift under us.

use std::ffi::{c_char, CStr, CString};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread;

use super::{AsrEngine, AsrError, AsrErrorKind, AsrEvent, EngineKind, EventSink, StartOptions, SAMPLE_RATE};

#[repr(C)]
struct FeatureConfig {
    sample_rate: i32,
    feature_dim: i32,
}

#[repr(C)]
struct TransducerConfig {
    encoder: *const c_char,
    decoder: *const c_char,
    joiner: *const c_char,
}

#[repr(C)]
struct ParaformerConfig {
    encoder: *const c_char,
    decoder: *const c_char,
}

/// zipformer2_ctc / nemo_ctc / t_one_ctc: one model path each.
#[repr(C)]
struct SingleModelConfig {
    model: *const c_char,
}

#[repr(C)]
struct ModelConfig {
    transducer: TransducerConfig,
    paraformer: ParaformerConfig,
    zipformer2_ctc: SingleModelConfig,
    tokens: *const c_char,
    num_threads: i32,
    provider: *const c_char,
    debug: i32,
    model_type: *const c_char,
    modeling_unit: *const c_char,
    bpe_vocab: *const c_char,
    tokens_buf: *const c_char,
    tokens_buf_size: i32,
    nemo_ctc: SingleModelConfig,
    t_one_ctc: SingleModelConfig,
}

#[repr(C)]
struct CtcFstDecoderConfig {
    graph: *const c_char,
    max_active: i32,
}

#[repr(C)]
struct HomophoneReplacerConfig {
    dict_dir: *const c_char,
    lexicon: *const c_char,
    rule_fsts: *const c_char,
}

#[repr(C)]
struct RecognizerConfig {
    feat_config: FeatureConfig,
    model_config: ModelConfig,
    decoding_method: *const c_char,
    max_active_paths: i32,
    enable_endpoint: i32,
    rule1_min_trailing_silence: f32,
    rule2_min_trailing_silence: f32,
    rule3_min_utterance_length: f32,
    hotwords_file: *const c_char,
    hotwords_score: f32,
    ctc_fst_decoder_config: CtcFstDecoderConfig,
    rule_fsts: *const c_char,
    rule_fars: *const c_char,
    blank_penalty: f32,
    hotwords_buf: *const c_char,
    hotwords_buf_size: i32,
    hr: HomophoneReplacerConfig,
}

#[repr(C)]
struct RecognizerResult {
    text: *const c_char,
    tokens: *const c_char,
    tokens_arr: *const *const c_char,
    timestamps: *mut f32,
    count: i32,
    json: *const c_char,
}

enum OpaqueRecognizer {}
enum OpaqueStream {}

type CreateRecognizer = unsafe extern "C" fn(*const RecognizerConfig) -> *const OpaqueRecognizer;
type DestroyRecognizer = unsafe extern "C" fn(*const OpaqueRecognizer);
type CreateStream = unsafe extern "C" fn(*const OpaqueRecognizer) -> *const OpaqueStream;
type DestroyStream = unsafe extern "C" fn(*const OpaqueStream);
type AcceptWaveform = unsafe extern "C" fn(*const OpaqueStream, i32, *const f32, i32);
type IsReady = unsafe extern "C" fn(*const OpaqueRecognizer, *const OpaqueStream) -> i32;
type Decode = unsafe extern "C" fn(*const OpaqueRecognizer, *const OpaqueStream);
type GetResult = unsafe extern "C" fn(*const OpaqueRecognizer, *const OpaqueStream) -> *const RecognizerResult;
type DestroyResult = unsafe extern "C" fn(*const RecognizerResult);
type InputFinished = unsafe extern "C" fn(*const OpaqueStream);

struct Api {
    create_recognizer: CreateRecognizer,
    destroy_recognizer: DestroyRecognizer,
    create_stream: CreateStream,
    destroy_stream: DestroyStream,
    accept_waveform: AcceptWaveform,
    is_ready: IsReady,
    decode: Decode,
    get_result: GetResult,
    destroy_result: DestroyResult,
    input_finished: InputFinished,
}

/// Where the runtime and the model files are.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LocalPaths {
    /// Folder holding the sherpa-onnx C API library and onnxruntime.
    pub runtime_dir: PathBuf,
    pub encoder: PathBuf,
    pub decoder: PathBuf,
    pub tokens: PathBuf,
}

#[cfg(target_os = "windows")]
const RUNTIME_LIBS: [&str; 2] = ["onnxruntime.dll", "sherpa-onnx-c-api.dll"];
#[cfg(target_os = "macos")]
const RUNTIME_LIBS: [&str; 2] = ["libonnxruntime.dylib", "libsherpa-onnx-c-api.dylib"];
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
const RUNTIME_LIBS: [&str; 2] = ["libonnxruntime.so", "libsherpa-onnx-c-api.so"];

/// A loaded model. Creating one reads ~240 MB, so it is kept for reuse.
pub struct Recognizer {
    api: Api,
    ptr: *const OpaqueRecognizer,
    // Keep the libraries loaded for as long as `ptr` lives (onnxruntime
    // first, so it outlives the C API library that depends on it).
    _libs: Vec<libloading::Library>,
}

// The recognizer is only used under the cache mutex / by one session at a time.
unsafe impl Send for Recognizer {}
unsafe impl Sync for Recognizer {}

impl Drop for Recognizer {
    fn drop(&mut self) {
        unsafe { (self.api.destroy_recognizer)(self.ptr) };
    }
}

fn load_lib(path: &Path) -> Result<libloading::Library, AsrError> {
    let err = |e: libloading::Error| AsrError::new(AsrErrorKind::NotInstalled, format!("load {}: {e}", path.display()));
    #[cfg(target_os = "windows")]
    {
        // Resolve the DLL's own imports from its folder, not the app folder.
        const LOAD_WITH_ALTERED_SEARCH_PATH: u32 = 0x0000_0008;
        unsafe { libloading::os::windows::Library::load_with_flags(path, LOAD_WITH_ALTERED_SEARCH_PATH) }
            .map(Into::into)
            .map_err(err)
    }
    #[cfg(not(target_os = "windows"))]
    {
        unsafe { libloading::Library::new(path) }.map_err(err)
    }
}

fn cstring(p: &Path) -> Result<CString, AsrError> {
    let s = p.to_str().ok_or_else(|| AsrError::new(AsrErrorKind::NotInstalled, "model path is not UTF-8"))?;
    CString::new(s).map_err(|_| AsrError::new(AsrErrorKind::NotInstalled, "model path contains NUL"))
}

impl Recognizer {
    pub fn load(paths: &LocalPaths) -> Result<Recognizer, AsrError> {
        for f in [&paths.encoder, &paths.decoder, &paths.tokens] {
            if !f.is_file() {
                return Err(AsrError::new(AsrErrorKind::NotInstalled, format!("missing {}", f.display())));
            }
        }
        let mut libs = Vec::new();
        for name in RUNTIME_LIBS {
            libs.push(load_lib(&paths.runtime_dir.join(name))?);
        }
        let api = {
            let lib = &libs[1];
            macro_rules! sym {
                ($name:literal) => {
                    *unsafe { lib.get($name) }
                        .map_err(|e| AsrError::new(AsrErrorKind::NotInstalled, format!("sherpa-onnx symbol: {e}")))?
                };
            }
            Api {
                create_recognizer: sym!(b"SherpaOnnxCreateOnlineRecognizer\0"),
                destroy_recognizer: sym!(b"SherpaOnnxDestroyOnlineRecognizer\0"),
                create_stream: sym!(b"SherpaOnnxCreateOnlineStream\0"),
                destroy_stream: sym!(b"SherpaOnnxDestroyOnlineStream\0"),
                accept_waveform: sym!(b"SherpaOnnxOnlineStreamAcceptWaveform\0"),
                is_ready: sym!(b"SherpaOnnxIsOnlineStreamReady\0"),
                decode: sym!(b"SherpaOnnxDecodeOnlineStream\0"),
                get_result: sym!(b"SherpaOnnxGetOnlineStreamResult\0"),
                destroy_result: sym!(b"SherpaOnnxDestroyOnlineRecognizerResult\0"),
                input_finished: sym!(b"SherpaOnnxOnlineStreamInputFinished\0"),
            }
        };
        let encoder = cstring(&paths.encoder)?;
        let decoder = cstring(&paths.decoder)?;
        let tokens = cstring(&paths.tokens)?;
        let provider = c"cpu";
        let greedy = c"greedy_search";
        let threads = thread::available_parallelism().map(|n| n.get()).unwrap_or(2).clamp(1, 4) as i32;
        let null = ptr::null();
        let config = RecognizerConfig {
            feat_config: FeatureConfig { sample_rate: SAMPLE_RATE as i32, feature_dim: 80 },
            model_config: ModelConfig {
                transducer: TransducerConfig { encoder: null, decoder: null, joiner: null },
                paraformer: ParaformerConfig { encoder: encoder.as_ptr(), decoder: decoder.as_ptr() },
                zipformer2_ctc: SingleModelConfig { model: null },
                tokens: tokens.as_ptr(),
                num_threads: threads,
                provider: provider.as_ptr(),
                debug: 0,
                model_type: null,
                modeling_unit: null,
                bpe_vocab: null,
                tokens_buf: null,
                tokens_buf_size: 0,
                nemo_ctc: SingleModelConfig { model: null },
                t_one_ctc: SingleModelConfig { model: null },
            },
            decoding_method: greedy.as_ptr(),
            max_active_paths: 4,
            // One utterance per key press: no endpointing inside it.
            enable_endpoint: 0,
            rule1_min_trailing_silence: 2.4,
            rule2_min_trailing_silence: 1.2,
            rule3_min_utterance_length: 300.0,
            hotwords_file: null,
            hotwords_score: 1.5,
            ctc_fst_decoder_config: CtcFstDecoderConfig { graph: null, max_active: 3000 },
            rule_fsts: null,
            rule_fars: null,
            blank_penalty: 0.0,
            hotwords_buf: null,
            hotwords_buf_size: 0,
            hr: HomophoneReplacerConfig { dict_dir: null, lexicon: null, rule_fsts: null },
        };
        let ptr = unsafe { (api.create_recognizer)(&config) };
        if ptr.is_null() {
            return Err(AsrError::new(AsrErrorKind::NotInstalled, "sherpa-onnx could not load the model"));
        }
        Ok(Recognizer { api, ptr, _libs: libs })
    }

    /// Decode one utterance. `audio` yields 16 kHz samples in [-1, 1] until
    /// it ends (None = finished, Err = cancelled); `partial` gets the text
    /// whenever it changes. Returns the final text.
    pub fn decode<F>(&self, audio: &Receiver<Option<Vec<f32>>>, mut partial: F) -> Result<String, ()>
    where
        F: FnMut(&str),
    {
        let stream = unsafe { (self.api.create_stream)(self.ptr) };
        if stream.is_null() {
            return Err(());
        }
        let mut last = String::new();
        let run = |samples: &[f32], last: &mut String, partial: &mut F| {
            if !samples.is_empty() {
                unsafe { (self.api.accept_waveform)(stream, SAMPLE_RATE as i32, samples.as_ptr(), samples.len() as i32) };
            }
            while unsafe { (self.api.is_ready)(self.ptr, stream) } != 0 {
                unsafe { (self.api.decode)(self.ptr, stream) };
            }
            let text = self.result(stream);
            if text != *last {
                partial(&text);
                *last = text;
            }
        };
        let outcome = loop {
            match audio.recv() {
                Ok(Some(samples)) => run(&samples, &mut last, &mut partial),
                Ok(None) => {
                    // Tail padding flushes the last chunk out of the
                    // streaming encoder (as in the official examples).
                    run(&vec![0.0; SAMPLE_RATE as usize * 4 / 5], &mut last, &mut partial);
                    unsafe { (self.api.input_finished)(stream) };
                    run(&[], &mut last, &mut partial);
                    break Ok(self.result(stream));
                }
                Err(_) => break Err(()),
            }
        };
        unsafe { (self.api.destroy_stream)(stream) };
        outcome
    }

    fn result(&self, stream: *const OpaqueStream) -> String {
        let r = unsafe { (self.api.get_result)(self.ptr, stream) };
        if r.is_null() {
            return String::new();
        }
        let text = unsafe {
            let t = (*r).text;
            if t.is_null() { String::new() } else { CStr::from_ptr(t).to_string_lossy().trim().to_string() }
        };
        unsafe { (self.api.destroy_result)(r) };
        text
    }
}

/// The loaded model, kept across utterances (and unloaded by `unload`).
static CACHE: Mutex<Option<(LocalPaths, Arc<Recognizer>)>> = Mutex::new(None);

pub fn recognizer(paths: &LocalPaths) -> Result<Arc<Recognizer>, AsrError> {
    let mut cache = CACHE.lock().map_err(|_| AsrError::new(AsrErrorKind::NotInstalled, "cache poisoned"))?;
    if let Some((p, r)) = cache.as_ref() {
        if p == paths {
            return Ok(r.clone());
        }
    }
    let r = Arc::new(Recognizer::load(paths)?);
    *cache = Some((paths.clone(), r.clone()));
    Ok(r)
}

/// Free the model memory (before deleting or replacing the files).
pub fn unload() {
    if let Ok(mut cache) = CACHE.lock() {
        *cache = None;
    }
}

/// Load the model in the background so the first key press is fast.
pub fn preload(paths: LocalPaths) {
    let _ = thread::Builder::new().name("pc-asr-local-load".into()).spawn(move || {
        let _ = recognizer(&paths);
    });
}

/// The local engine.
pub struct SherpaEngine {
    paths: LocalPaths,
    tx: Option<Sender<Option<Vec<f32>>>>,
}

pub fn engine(paths: LocalPaths) -> Box<dyn AsrEngine> {
    Box::new(SherpaEngine { paths, tx: None })
}

impl AsrEngine for SherpaEngine {
    fn kind(&self) -> EngineKind {
        EngineKind::Local
    }

    fn start(&mut self, _opts: &StartOptions, events: EventSink) -> Result<(), AsrError> {
        // shortcut: the streaming Paraformer has no hotword biasing in
        // sherpa-onnx; switch to a transducer model if hotwords matter here.
        let (tx, rx) = mpsc::channel();
        let paths = self.paths.clone();
        thread::Builder::new()
            .name("pc-asr-local".into())
            .spawn(move || {
                let rec = match recognizer(&paths) {
                    Ok(r) => r,
                    Err(e) => return events(AsrEvent::Error(e)),
                };
                if let Ok(text) = rec.decode(&rx, |t| events(AsrEvent::Partial(t.to_string()))) {
                    events(AsrEvent::Final(text));
                }
            })
            .map_err(|e| AsrError::new(AsrErrorKind::NotInstalled, format!("spawn: {e}")))?;
        self.tx = Some(tx);
        Ok(())
    }

    fn push_audio(&mut self, pcm: &[i16]) -> Result<(), AsrError> {
        if let Some(tx) = &self.tx {
            let _ = tx.send(Some(pcm.iter().map(|&s| s as f32 / 32768.0).collect()));
        }
        Ok(())
    }

    fn finish(&mut self) -> Result<(), AsrError> {
        if let Some(tx) = &self.tx {
            let _ = tx.send(None);
        }
        Ok(())
    }

    fn cancel(&mut self) {
        // Dropping the sender ends the decode loop without a Final.
        self.tx = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    /// Offsets on 64-bit targets, computed by hand from c-api.h v2.0.1
    /// (8-byte pointers, 4-byte int32/float, natural alignment). Windows x64
    /// and Linux x86_64 agree because the header uses no `long`.
    #[cfg(target_pointer_width = "64")]
    #[test]
    fn struct_layout_matches_the_c_header() {
        assert_eq!(size_of::<FeatureConfig>(), 8);
        assert_eq!(size_of::<ModelConfig>(), 8 * 3 + 8 * 2 + 8 + 8 + 8 + 8 + 8 + 8 * 4 + 8 + 8 + 8);
        assert_eq!(offset_of!(ModelConfig, tokens), 48);
        assert_eq!(offset_of!(ModelConfig, num_threads), 56);
        assert_eq!(offset_of!(ModelConfig, provider), 64);
        assert_eq!(offset_of!(ModelConfig, tokens_buf_size), 112);
        assert_eq!(offset_of!(ModelConfig, nemo_ctc), 120);
        assert_eq!(offset_of!(RecognizerConfig, model_config), 8);
        assert_eq!(offset_of!(RecognizerConfig, decoding_method), 8 + size_of::<ModelConfig>());
        assert_eq!(offset_of!(RecognizerConfig, hotwords_file), 8 + size_of::<ModelConfig>() + 8 + 4 * 5 + 4);
        assert_eq!(size_of::<RecognizerResult>(), 48);
        assert_eq!(align_of::<RecognizerConfig>(), 8);
    }

    #[test]
    fn missing_model_is_not_installed() {
        let p = LocalPaths {
            runtime_dir: PathBuf::from("/nonexistent"),
            encoder: PathBuf::from("/nonexistent/e.onnx"),
            decoder: PathBuf::from("/nonexistent/d.onnx"),
            tokens: PathBuf::from("/nonexistent/t.txt"),
        };
        assert_eq!(Recognizer::load(&p).err().map(|e| e.kind), Some(AsrErrorKind::NotInstalled));
        let mut eng = engine(p);
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        let ev = rx.recv_timeout(std::time::Duration::from_secs(5)).unwrap();
        assert!(matches!(ev, AsrEvent::Error(e) if e.kind == AsrErrorKind::NotInstalled));
    }

    /// Real model + runtime (not run by default):
    /// `SHERPA_ONNX_LIB_DIR=<dir with libsherpa-onnx-c-api.so> SHERPA_MODEL_DIR=<model dir>
    ///  cargo test --lib real_model -- --ignored`
    #[test]
    #[ignore]
    fn real_model_decodes_a_recorded_wav() {
        let lib = PathBuf::from(std::env::var("SHERPA_ONNX_LIB_DIR").expect("SHERPA_ONNX_LIB_DIR"));
        let dir = PathBuf::from(std::env::var("SHERPA_MODEL_DIR").expect("SHERPA_MODEL_DIR"));
        let paths = LocalPaths {
            runtime_dir: lib,
            encoder: dir.join("encoder.int8.onnx"),
            decoder: dir.join("decoder.int8.onnx"),
            tokens: dir.join("tokens.txt"),
        };
        let pcm = super::super::tests_wav::read_wav_16k_mono(&dir.join("test_wavs/1.wav"));
        let mut eng = engine(paths);
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        for chunk in pcm.chunks(1600) {
            eng.push_audio(chunk).unwrap();
        }
        eng.finish().unwrap();
        let mut partials = 0;
        let text = loop {
            match rx.recv_timeout(std::time::Duration::from_secs(60)).unwrap() {
                AsrEvent::Partial(_) => partials += 1,
                AsrEvent::Final(t) => break t,
                AsrEvent::Error(e) => panic!("{e}"),
            }
        };
        eprintln!("final: {text} ({partials} partials)");
        assert!(partials > 0);
        assert!(text.chars().count() >= 4, "{text}");
    }
}
