//! PC streaming speech recognition ("PC ASR", docs/pc-asr.md).
//!
//! When the keyboard's voice route is `system`, holding the voice key sends
//! `voice:start` / `voice:stop`. Between the two the Companion records the PC
//! microphone and streams it to one engine:
//!
//! - [`doubao`]: Volcengine Doubao streaming ASR (cloud, WebSocket);
//! - [`aliyun`]: Alibaba Cloud Model Studio Fun-ASR / Paraformer realtime
//!   (cloud, WebSocket);
//! - [`sherpa`]: sherpa-onnx streaming Paraformer (local, offline).
//!
//! The final text is NOT typed anywhere: it goes back to the keyboard's input
//! confirm ③ (`input_restore`), exactly like text from the keyboard's own cloud
//! ASR, and K6 sends it. This module is pure apart from the engines' IO, so the
//! session logic (fallback, replay, timeouts) is tested with mock engines.

pub mod aliyun;
pub mod capture;
pub mod doubao;
pub mod model;
pub mod settings;
pub mod sherpa;
mod ws;

use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::thread;
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

pub use settings::AsrSettings;

/// Every engine takes 16 kHz, mono, 16-bit PCM.
pub const SAMPLE_RATE: u32 = 16_000;
/// Longest utterance kept for a fallback replay (and the longest recording).
pub const MAX_UTTERANCE_SECS: usize = 120;
const MAX_BUFFER_SAMPLES: usize = MAX_UTTERANCE_SECS * SAMPLE_RATE as usize;

/// The three engines.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EngineKind {
    #[default]
    Doubao,
    Aliyun,
    Local,
}

impl EngineKind {
    pub fn as_str(self) -> &'static str {
        match self {
            EngineKind::Doubao => "doubao",
            EngineKind::Aliyun => "aliyun",
            EngineKind::Local => "local",
        }
    }

    pub fn parse(s: &str) -> Option<EngineKind> {
        match s.trim() {
            "doubao" => Some(EngineKind::Doubao),
            "aliyun" => Some(EngineKind::Aliyun),
            "local" => Some(EngineKind::Local),
            _ => None,
        }
    }

    pub fn is_cloud(self) -> bool {
        self != EngineKind::Local
    }
}

/// Why an engine stopped. `detail` is for the Companion log; it never holds
/// a key (engines only ever put server messages and IO errors in it).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AsrError {
    pub kind: AsrErrorKind,
    pub detail: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AsrErrorKind {
    /// No API key stored for a cloud engine.
    NoKey,
    /// The service refused the key.
    Auth,
    /// Could not connect, or the connection dropped.
    Network,
    /// The service answered with something this client does not understand.
    Protocol,
    /// The service reported an error.
    Server,
    /// No final result in time.
    Timeout,
    /// The local model is not downloaded (or cannot be loaded).
    NotInstalled,
    /// Microphone capture failed.
    Audio,
}

impl AsrError {
    pub fn new(kind: AsrErrorKind, detail: impl Into<String>) -> Self {
        Self { kind, detail: detail.into() }
    }

    /// Short English text for the keyboard status bar (through `lcd_text`).
    pub fn short(&self) -> &'static str {
        match self.kind {
            AsrErrorKind::NoKey => "PC ASR: no API key",
            AsrErrorKind::Auth => "PC ASR: key refused",
            AsrErrorKind::Network => "PC ASR: network error",
            AsrErrorKind::Protocol | AsrErrorKind::Server => "PC ASR failed",
            AsrErrorKind::Timeout => "PC ASR timed out",
            AsrErrorKind::NotInstalled => "Local model missing",
            AsrErrorKind::Audio => "Microphone error",
        }
    }
}

impl std::fmt::Display for AsrError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.detail)
    }
}

/// What an engine reports while it runs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AsrEvent {
    /// The whole text so far (replaces the previous partial).
    Partial(String),
    /// The final text of the utterance; the engine is done.
    Final(String),
    /// The engine stopped; no Final follows.
    Error(AsrError),
}

/// Where an engine sends its events, from its own worker thread.
pub type EventSink = Box<dyn Fn(AsrEvent) + Send + 'static>;

/// Per-utterance options.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StartOptions {
    /// Words to bias recognition towards (empty: none).
    pub hotwords: Vec<String>,
}

/// One streaming recognizer: start, push audio, finish (or cancel); partial
/// and final results and errors arrive on the sink given to `start`.
pub trait AsrEngine: Send {
    fn kind(&self) -> EngineKind;
    /// Open the session. Connecting happens on the engine's worker thread, so
    /// this returns at once; a failure to connect arrives as `Error`.
    fn start(&mut self, opts: &StartOptions, events: EventSink) -> Result<(), AsrError>;
    /// 16 kHz mono PCM. Must not block on the network.
    fn push_audio(&mut self, pcm: &[i16]) -> Result<(), AsrError>;
    /// No more audio: the engine sends `Final` (or `Error`) and stops.
    fn finish(&mut self) -> Result<(), AsrError>;
    /// Stop now; no further events.
    fn cancel(&mut self);
}

/// Builds an engine of the given kind (looks up keys, settings, the model).
pub type EngineFactory = Box<dyn Fn(EngineKind) -> Result<Box<dyn AsrEngine>, AsrError> + Send>;

/// What a session reports to the app.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SessionOutput {
    Partial { engine: EngineKind, text: String },
    /// Done. `text` may be empty (nothing recognised).
    Final { engine: EngineKind, text: String },
    /// `from` failed with `reason`; the recorded audio is replayed into `to`.
    FellBack { from: EngineKind, to: EngineKind, reason: AsrError },
    /// Done without text.
    Failed { engine: EngineKind, reason: AsrError },
}

enum Msg {
    Audio(Vec<i16>),
    Stop,
    Cancel,
    Engine(u32, AsrEvent),
}

/// Feeds microphone audio into a running session (cheap to clone).
#[derive(Clone)]
pub struct AudioFeeder(Sender<Msg>);

impl AudioFeeder {
    pub fn push(&self, pcm: Vec<i16>) {
        let _ = self.0.send(Msg::Audio(pcm));
    }
}

/// A running utterance. Dropping it does not stop it; call `stop` or `cancel`.
pub struct Session {
    tx: Sender<Msg>,
}

impl Session {
    /// Start an utterance on `primary`; on any failure fall back once to
    /// `fallback` (replaying the audio recorded so far). `output` runs on the
    /// session thread. After `stop`, the final result must come within
    /// `final_timeout`.
    pub fn start(
        primary: EngineKind,
        fallback: Option<EngineKind>,
        opts: StartOptions,
        factory: EngineFactory,
        output: Box<dyn Fn(SessionOutput) + Send>,
        final_timeout: Duration,
    ) -> Session {
        let (tx, rx) = mpsc::channel();
        let runner = Runner {
            tx: tx.clone(),
            factory,
            output,
            opts,
            kind: primary,
            fallback: fallback.filter(|f| *f != primary),
            engine: None,
            generation: 0,
            buffer: Vec::new(),
            stopped: false,
            deadline: None,
            final_timeout,
        };
        let _ = thread::Builder::new().name("pc-asr-session".into()).spawn(move || runner.run(rx));
        Session { tx }
    }

    pub fn feeder(&self) -> AudioFeeder {
        AudioFeeder(self.tx.clone())
    }

    /// End of speech: wait for the final result.
    pub fn stop(&self) {
        let _ = self.tx.send(Msg::Stop);
    }

    /// Drop the utterance; no output follows.
    pub fn cancel(&self) {
        let _ = self.tx.send(Msg::Cancel);
    }
}

struct Runner {
    tx: Sender<Msg>,
    factory: EngineFactory,
    output: Box<dyn Fn(SessionOutput) + Send>,
    opts: StartOptions,
    kind: EngineKind,
    fallback: Option<EngineKind>,
    engine: Option<Box<dyn AsrEngine>>,
    generation: u32,
    buffer: Vec<i16>,
    stopped: bool,
    deadline: Option<Instant>,
    final_timeout: Duration,
}

/// Whether the session goes on after handling a message.
#[derive(PartialEq)]
enum Flow {
    Go,
    Done,
}

impl Runner {
    fn run(mut self, rx: Receiver<Msg>) {
        if self.open(self.kind) == Flow::Done {
            return;
        }
        loop {
            let msg = match self.deadline {
                Some(deadline) => match rx.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                    Ok(msg) => msg,
                    Err(RecvTimeoutError::Timeout) => {
                        let e = AsrError::new(AsrErrorKind::Timeout, "no final result");
                        if self.fail(e) == Flow::Done {
                            return;
                        }
                        continue;
                    }
                    Err(RecvTimeoutError::Disconnected) => return,
                },
                None => match rx.recv() {
                    Ok(msg) => msg,
                    Err(_) => return,
                },
            };
            if self.handle(msg) == Flow::Done {
                return;
            }
        }
    }

    fn handle(&mut self, msg: Msg) -> Flow {
        match msg {
            Msg::Audio(pcm) => {
                if self.stopped {
                    return Flow::Go;
                }
                let room = MAX_BUFFER_SAMPLES.saturating_sub(self.buffer.len());
                let pcm = &pcm[..pcm.len().min(room)];
                if pcm.is_empty() {
                    return Flow::Go;
                }
                self.buffer.extend_from_slice(pcm);
                match self.engine.as_mut().map(|e| e.push_audio(pcm)) {
                    Some(Err(e)) => self.fail(e),
                    _ => Flow::Go,
                }
            }
            Msg::Stop => {
                if self.stopped {
                    return Flow::Go;
                }
                self.stopped = true;
                if self.buffer.is_empty() {
                    // Nothing recorded: no need to ask a service.
                    self.close();
                    (self.output)(SessionOutput::Final { engine: self.kind, text: String::new() });
                    return Flow::Done;
                }
                self.deadline = Some(Instant::now() + self.final_timeout);
                match self.engine.as_mut().map(|e| e.finish()) {
                    Some(Err(e)) => self.fail(e),
                    _ => Flow::Go,
                }
            }
            Msg::Cancel => {
                self.close();
                Flow::Done
            }
            Msg::Engine(generation, _) if generation != self.generation => Flow::Go,
            Msg::Engine(_, AsrEvent::Partial(text)) => {
                (self.output)(SessionOutput::Partial { engine: self.kind, text });
                Flow::Go
            }
            Msg::Engine(_, AsrEvent::Final(text)) => {
                self.engine = None;
                (self.output)(SessionOutput::Final { engine: self.kind, text: text.trim().to_string() });
                Flow::Done
            }
            Msg::Engine(_, AsrEvent::Error(e)) => self.fail(e),
        }
    }

    /// Start `kind`; on failure fall back (or give up).
    fn open(&mut self, kind: EngineKind) -> Flow {
        self.kind = kind;
        self.generation = self.generation.wrapping_add(1);
        let generation = self.generation;
        let tx = self.tx.clone();
        let sink: EventSink = Box::new(move |ev| {
            let _ = tx.send(Msg::Engine(generation, ev));
        });
        let started = (self.factory)(kind).and_then(|mut engine| {
            engine.start(&self.opts, sink)?;
            Ok(engine)
        });
        let mut engine = match started {
            Ok(engine) => engine,
            Err(e) => return self.fail(e),
        };
        // A fallback engine gets everything recorded so far.
        for chunk in self.buffer.chunks(SAMPLE_RATE as usize / 5) {
            if let Err(e) = engine.push_audio(chunk) {
                engine.cancel();
                return self.fail(e);
            }
        }
        if self.stopped {
            self.deadline = Some(Instant::now() + self.final_timeout);
            if let Err(e) = engine.finish() {
                engine.cancel();
                return self.fail(e);
            }
        }
        self.engine = Some(engine);
        Flow::Go
    }

    fn fail(&mut self, reason: AsrError) -> Flow {
        self.close();
        match self.fallback.take() {
            Some(next) => {
                (self.output)(SessionOutput::FellBack { from: self.kind, to: next, reason });
                self.open(next)
            }
            None => {
                (self.output)(SessionOutput::Failed { engine: self.kind, reason });
                Flow::Done
            }
        }
    }

    fn close(&mut self) {
        if let Some(mut engine) = self.engine.take() {
            engine.cancel();
        }
        // Ignore anything the closed engine still sends.
        self.generation = self.generation.wrapping_add(1);
    }
}

/// Microphone samples (any rate, interleaved channels) -> 16 kHz mono i16.
/// Each output sample is the mean of the input samples it covers (a box
/// low-pass), which is enough for speech from a 44.1/48 kHz device.
#[derive(Clone, Debug)]
pub struct Resampler {
    channels: usize,
    /// Input samples per output sample.
    step: f64,
    phase: f64,
    acc: f64,
    count: u32,
    last: f64,
}

impl Resampler {
    pub fn new(in_rate: u32, channels: u16) -> Self {
        Self {
            channels: channels.max(1) as usize,
            step: in_rate.max(1) as f64 / SAMPLE_RATE as f64,
            phase: 0.0,
            acc: 0.0,
            count: 0,
            last: 0.0,
        }
    }

    /// `frames`: interleaved f32 samples in [-1, 1].
    pub fn push(&mut self, frames: &[f32], out: &mut Vec<i16>) {
        for frame in frames.chunks(self.channels) {
            let mono = frame.iter().map(|&s| s as f64).sum::<f64>() / frame.len() as f64;
            self.acc += mono;
            self.count += 1;
            self.phase += 1.0;
            while self.phase >= self.step {
                self.phase -= self.step;
                let v = if self.count > 0 { self.acc / self.count as f64 } else { self.last };
                self.last = v;
                self.acc = 0.0;
                self.count = 0;
                out.push((v.clamp(-1.0, 1.0) * i16::MAX as f64).round() as i16);
            }
        }
    }
}

/// PCM i16 -> little-endian bytes (the cloud wire format).
pub fn pcm_bytes(pcm: &[i16]) -> Vec<u8> {
    pcm.iter().flat_map(|s| s.to_le_bytes()).collect()
}

/// Keychain account holding a cloud engine's key.
pub fn key_account(kind: EngineKind) -> Option<&'static str> {
    match kind {
        EngineKind::Doubao => Some("asr.doubao"),
        EngineKind::Aliyun => Some("asr.aliyun"),
        EngineKind::Local => None,
    }
}

/// Build an engine from the settings, the keychain and the local model.
pub fn build_engine(
    kind: EngineKind,
    settings: &AsrSettings,
    secrets: &dyn crate::secrets::SecretStore,
) -> Result<Box<dyn AsrEngine>, AsrError> {
    let key = || -> Result<String, AsrError> {
        let account = key_account(kind).unwrap_or_default();
        secrets
            .get(account)
            .map_err(|e| AsrError::new(AsrErrorKind::NoKey, e))?
            .filter(|k| !k.is_empty())
            .ok_or_else(|| AsrError::new(AsrErrorKind::NoKey, format!("no {} key", kind.as_str())))
    };
    match kind {
        EngineKind::Doubao => Ok(doubao::engine(doubao::Config {
            url: doubao::URL.to_string(),
            key: key()?,
            app_id: settings.doubao_app_id.clone(),
            resource_id: settings.doubao_resource.clone(),
        })),
        EngineKind::Aliyun => Ok(aliyun::engine(aliyun::Config {
            url: aliyun::URL.to_string(),
            key: key()?,
            model: settings.aliyun_model.clone(),
        })),
        EngineKind::Local => {
            if !model::status().installed {
                return Err(AsrError::new(AsrErrorKind::NotInstalled, "local model not downloaded"));
            }
            Ok(sherpa::engine(model::paths()))
        }
    }
}

/// Random lowercase hex id (task / connect ids) without a uuid feature.
pub(crate) fn random_id() -> String {
    let mut b = [0u8; 16];
    if getrandom::getrandom(&mut b).is_err() {
        let t = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        b = t.to_le_bytes();
    }
    b.iter().map(|x| format!("{x:02x}")).collect()
}

#[cfg(test)]
pub(crate) mod tests_wav {
    /// 16-bit PCM WAV -> samples (first channel), for tests with recorded audio.
    pub fn read_wav_16k_mono(path: &std::path::Path) -> Vec<i16> {
        let b = std::fs::read(path).expect("wav");
        assert_eq!(&b[0..4], b"RIFF");
        let mut at = 12;
        let (mut channels, mut data) = (1usize, &b[0..0]);
        while at + 8 <= b.len() {
            let id = &b[at..at + 4];
            let len = u32::from_le_bytes([b[at + 4], b[at + 5], b[at + 6], b[at + 7]]) as usize;
            let body = &b[at + 8..(at + 8 + len).min(b.len())];
            if id == b"fmt " {
                channels = u16::from_le_bytes([body[2], body[3]]) as usize;
                assert_eq!(u32::from_le_bytes([body[4], body[5], body[6], body[7]]), super::SAMPLE_RATE);
                assert_eq!(u16::from_le_bytes([body[14], body[15]]), 16);
            } else if id == b"data" {
                data = body;
            }
            at += 8 + len + (len & 1);
        }
        data.chunks_exact(2 * channels).map(|f| i16::from_le_bytes([f[0], f[1]])).collect()
    }
}

#[cfg(test)]
pub(crate) mod testutil {
    use super::*;
    use std::sync::{Arc, Mutex};

    /// What a mock engine was asked to do.
    #[derive(Clone, Debug, Default)]
    pub struct Calls {
        pub started: bool,
        pub samples: usize,
        pub finished: bool,
        pub cancelled: bool,
    }

    /// Scripted engine: fails where told, else answers `text` on finish.
    pub struct MockEngine {
        pub kind: EngineKind,
        pub fail_start: Option<AsrErrorKind>,
        pub error_after_samples: Option<usize>,
        pub text: String,
        pub calls: Arc<Mutex<Calls>>,
        pub sink: Option<EventSink>,
        pub silent: bool,
    }

    impl AsrEngine for MockEngine {
        fn kind(&self) -> EngineKind {
            self.kind
        }
        fn start(&mut self, _: &StartOptions, events: EventSink) -> Result<(), AsrError> {
            if let Some(kind) = self.fail_start {
                return Err(AsrError::new(kind, "mock start"));
            }
            self.calls.lock().unwrap().started = true;
            self.sink = Some(events);
            Ok(())
        }
        fn push_audio(&mut self, pcm: &[i16]) -> Result<(), AsrError> {
            let total = {
                let mut c = self.calls.lock().unwrap();
                c.samples += pcm.len();
                c.samples
            };
            if let (Some(limit), Some(sink)) = (self.error_after_samples, &self.sink) {
                if total >= limit {
                    sink(AsrEvent::Error(AsrError::new(AsrErrorKind::Network, "mock drop")));
                    return Ok(());
                }
            }
            if let Some(sink) = &self.sink {
                sink(AsrEvent::Partial(format!("{}…", self.text)));
            }
            Ok(())
        }
        fn finish(&mut self) -> Result<(), AsrError> {
            self.calls.lock().unwrap().finished = true;
            if !self.silent {
                if let Some(sink) = &self.sink {
                    sink(AsrEvent::Final(self.text.clone()));
                }
            }
            Ok(())
        }
        fn cancel(&mut self) {
            self.calls.lock().unwrap().cancelled = true;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use std::collections::HashMap;
    use std::sync::{Arc, Mutex};

    struct Plan {
        fail_start: Option<AsrErrorKind>,
        error_after: Option<usize>,
        silent: bool,
        text: &'static str,
    }

    fn plan(text: &'static str) -> Plan {
        Plan { fail_start: None, error_after: None, silent: false, text }
    }

    fn run(
        primary: EngineKind,
        fallback: Option<EngineKind>,
        plans: Vec<(EngineKind, Plan)>,
        script: impl FnOnce(&Session),
    ) -> (Vec<SessionOutput>, HashMap<EngineKind, Calls>) {
        let calls: HashMap<EngineKind, Arc<Mutex<Calls>>> =
            plans.iter().map(|(k, _)| (*k, Arc::new(Mutex::new(Calls::default())))).collect();
        let plans: HashMap<EngineKind, Plan> = plans.into_iter().collect();
        let plans = Arc::new(plans);
        let calls_for_factory = calls.clone();
        let factory: EngineFactory = Box::new(move |kind| {
            let p = plans.get(&kind).expect("planned engine");
            Ok(Box::new(MockEngine {
                kind,
                fail_start: p.fail_start,
                error_after_samples: p.error_after,
                text: p.text.to_string(),
                calls: calls_for_factory[&kind].clone(),
                sink: None,
                silent: p.silent,
            }) as Box<dyn AsrEngine>)
        });
        let (tx, rx) = mpsc::channel();
        let output: Box<dyn Fn(SessionOutput) + Send> = Box::new(move |o| {
            let _ = tx.send(o);
        });
        let session = Session::start(primary, fallback, StartOptions::default(), factory, output, Duration::from_millis(300));
        script(&session);
        let mut outs = Vec::new();
        while let Ok(o) = rx.recv_timeout(Duration::from_secs(2)) {
            let done = matches!(o, SessionOutput::Final { .. } | SessionOutput::Failed { .. });
            outs.push(o);
            if done {
                break;
            }
        }
        let calls = calls.into_iter().map(|(k, c)| (k, c.lock().unwrap().clone())).collect();
        (outs, calls)
    }

    fn last(outs: &[SessionOutput]) -> &SessionOutput {
        outs.last().expect("an output")
    }

    fn speak(s: &Session) {
        let f = s.feeder();
        for _ in 0..5 {
            f.push(vec![100; 1600]);
        }
        s.stop();
    }

    #[test]
    fn primary_engine_gives_the_final_text() {
        let (outs, calls) = run(EngineKind::Doubao, Some(EngineKind::Local), vec![(EngineKind::Doubao, plan(" 你好 "))], speak);
        assert_eq!(last(&outs), &SessionOutput::Final { engine: EngineKind::Doubao, text: "你好".into() });
        assert!(outs.iter().any(|o| matches!(o, SessionOutput::Partial { .. })));
        assert_eq!(calls[&EngineKind::Doubao].samples, 8000);
        assert!(calls[&EngineKind::Doubao].finished);
    }

    #[test]
    fn missing_key_falls_back_to_local_with_all_audio() {
        let mut doubao = plan("x");
        doubao.fail_start = Some(AsrErrorKind::NoKey);
        let (outs, calls) = run(
            EngineKind::Doubao,
            Some(EngineKind::Local),
            vec![(EngineKind::Doubao, doubao), (EngineKind::Local, plan("本地"))],
            speak,
        );
        assert!(matches!(&outs[0], SessionOutput::FellBack { from: EngineKind::Doubao, to: EngineKind::Local, reason } if reason.kind == AsrErrorKind::NoKey));
        assert_eq!(last(&outs), &SessionOutput::Final { engine: EngineKind::Local, text: "本地".into() });
        assert_eq!(calls[&EngineKind::Local].samples, 8000);
    }

    #[test]
    fn mid_stream_drop_replays_the_recorded_audio_into_the_fallback() {
        let mut aliyun = plan("cloud");
        aliyun.error_after = Some(3200);
        let (outs, calls) = run(
            EngineKind::Aliyun,
            Some(EngineKind::Local),
            vec![(EngineKind::Aliyun, aliyun), (EngineKind::Local, plan("local"))],
            speak,
        );
        assert_eq!(last(&outs), &SessionOutput::Final { engine: EngineKind::Local, text: "local".into() });
        assert!(calls[&EngineKind::Aliyun].cancelled);
        // Every sample reaches the local engine exactly once (replay + live).
        assert_eq!(calls[&EngineKind::Local].samples, 8000);
    }

    #[test]
    fn no_fallback_reports_failure() {
        let mut doubao = plan("x");
        doubao.fail_start = Some(AsrErrorKind::Auth);
        let (outs, _) = run(EngineKind::Doubao, None, vec![(EngineKind::Doubao, doubao)], speak);
        assert!(matches!(last(&outs), SessionOutput::Failed { engine: EngineKind::Doubao, reason } if reason.kind == AsrErrorKind::Auth));
    }

    #[test]
    fn fallback_to_itself_is_ignored_and_fallback_happens_once() {
        let mut local = plan("x");
        local.fail_start = Some(AsrErrorKind::NotInstalled);
        let (outs, _) = run(EngineKind::Local, Some(EngineKind::Local), vec![(EngineKind::Local, local)], speak);
        assert_eq!(outs.len(), 1);
        assert!(matches!(last(&outs), SessionOutput::Failed { engine: EngineKind::Local, .. }));
    }

    #[test]
    fn silent_engine_times_out_then_falls_back() {
        let mut doubao = plan("x");
        doubao.silent = true;
        let (outs, _) = run(
            EngineKind::Doubao,
            Some(EngineKind::Local),
            vec![(EngineKind::Doubao, doubao), (EngineKind::Local, plan("ok"))],
            speak,
        );
        assert!(outs.iter().any(|o| matches!(o, SessionOutput::FellBack { reason, .. } if reason.kind == AsrErrorKind::Timeout)));
        assert_eq!(last(&outs), &SessionOutput::Final { engine: EngineKind::Local, text: "ok".into() });
    }

    #[test]
    fn stop_without_audio_is_an_empty_final_and_cancel_is_silent() {
        let (outs, calls) = run(EngineKind::Doubao, None, vec![(EngineKind::Doubao, plan("x"))], |s| s.stop());
        assert_eq!(outs, vec![SessionOutput::Final { engine: EngineKind::Doubao, text: String::new() }]);
        assert!(!calls[&EngineKind::Doubao].finished);

        let (outs, calls) = run(EngineKind::Doubao, None, vec![(EngineKind::Doubao, plan("x"))], |s| {
            s.feeder().push(vec![1; 160]);
            s.cancel();
        });
        assert!(outs.iter().all(|o| matches!(o, SessionOutput::Partial { .. })));
        assert!(calls[&EngineKind::Doubao].cancelled);
    }

    #[test]
    fn resampler_downmixes_and_converts_rates() {
        let mut r = Resampler::new(48_000, 2);
        let mut out = Vec::new();
        // 0.1 s of stereo full-scale 0.5 -> 1600 mono samples at 16 kHz.
        r.push(&vec![0.5f32; 4800 * 2], &mut out);
        assert_eq!(out.len(), 1600);
        assert!(out.iter().all(|&s| (s - 16384).abs() <= 1));

        let mut r = Resampler::new(44_100, 1);
        let mut out = Vec::new();
        for _ in 0..10 {
            r.push(&vec![-1.0f32; 4410], &mut out);
        }
        assert!((out.len() as i64 - 16_000).abs() <= 1, "{}", out.len());
        assert!(out.iter().all(|&s| s == -i16::MAX));

        let mut r = Resampler::new(8_000, 1);
        let mut out = Vec::new();
        r.push(&[0.25f32; 800], &mut out);
        assert_eq!(out.len(), 1600);
    }

    #[test]
    fn engine_names_round_trip() {
        for k in [EngineKind::Doubao, EngineKind::Aliyun, EngineKind::Local] {
            assert_eq!(EngineKind::parse(k.as_str()), Some(k));
        }
        assert_eq!(EngineKind::parse("system"), None);
        assert_eq!(pcm_bytes(&[1, -2]), vec![1, 0, 0xfe, 0xff]);
        assert_eq!(random_id().len(), 32);
        assert_ne!(random_id(), random_id());
    }
}
