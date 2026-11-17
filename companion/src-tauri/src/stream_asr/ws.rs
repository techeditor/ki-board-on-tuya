//! The WebSocket worker shared by the cloud engines. One thread per
//! utterance owns the socket: it connects, sends the protocol's opening
//! message, streams audio in fixed chunks, and turns server messages into
//! [`AsrEvent`]s. A short socket read timeout lets one thread both read and
//! send (tungstenite is blocking), the same pattern as the Crew API client.

use std::io::{ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use tungstenite::client::IntoClientRequest;
use tungstenite::handshake::client::Request;
use tungstenite::http::HeaderValue;
use tungstenite::{Error as WsError, HandshakeError, Message};

use super::{AsrEngine, AsrError, AsrErrorKind, AsrEvent, EngineKind, EventSink, StartOptions};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(8);
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
const POLL: Duration = Duration::from_millis(20);
/// From the opening message to the service accepting audio.
const START_TIMEOUT: Duration = Duration::from_secs(10);
/// Hard cap for one utterance on the wire.
const SESSION_TIMEOUT: Duration = Duration::from_secs(super::MAX_UTTERANCE_SECS as u64 + 30);

/// What a server message meant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum WireEvent {
    /// The service accepts audio now.
    Started,
    Partial(String),
    Final(String),
}

/// The per-service part of a cloud engine. Pure: unit-tested without a socket.
pub(crate) trait WireProtocol: Send + 'static {
    /// URL and headers for the handshake (credentials go in headers only).
    fn request(&self) -> Result<Request, AsrError>;
    /// Messages sent right after the handshake.
    fn open(&mut self, opts: &StartOptions) -> Vec<Message>;
    /// Whether audio may be sent yet.
    fn can_stream(&self) -> bool;
    /// Audio bytes per message.
    fn chunk_bytes(&self) -> usize;
    fn audio(&mut self, pcm: Vec<u8>) -> Vec<Message>;
    /// End of audio, with the bytes not yet sent.
    fn finish(&mut self, rest: Vec<u8>) -> Vec<Message>;
    fn handle(&mut self, msg: Message) -> Result<Option<WireEvent>, AsrError>;
}

enum Cmd {
    Audio(Vec<u8>),
    Finish,
}

/// An [`AsrEngine`] over a [`WireProtocol`].
pub(crate) struct WsEngine<P: WireProtocol> {
    kind: EngineKind,
    proto: Option<P>,
    tx: Option<Sender<Cmd>>,
    cancel: Arc<AtomicBool>,
}

impl<P: WireProtocol> WsEngine<P> {
    pub(crate) fn new(kind: EngineKind, proto: P) -> Self {
        Self { kind, proto: Some(proto), tx: None, cancel: Arc::new(AtomicBool::new(false)) }
    }
}

impl<P: WireProtocol> AsrEngine for WsEngine<P> {
    fn kind(&self) -> EngineKind {
        self.kind
    }

    fn start(&mut self, opts: &StartOptions, events: EventSink) -> Result<(), AsrError> {
        let proto = self.proto.take().ok_or_else(|| AsrError::new(AsrErrorKind::Protocol, "engine already started"))?;
        let request = proto.request()?;
        let (tx, rx) = mpsc::channel();
        let cancel = self.cancel.clone();
        let opts = opts.clone();
        thread::Builder::new()
            .name(format!("pc-asr-{}", self.kind.as_str()))
            .spawn(move || {
                if let Err(e) = worker(proto, request, &opts, rx, &cancel, &events) {
                    if !cancel.load(Ordering::Acquire) {
                        events(AsrEvent::Error(e));
                    }
                }
            })
            .map_err(|e| AsrError::new(AsrErrorKind::Network, format!("spawn worker: {e}")))?;
        self.tx = Some(tx);
        Ok(())
    }

    fn push_audio(&mut self, pcm: &[i16]) -> Result<(), AsrError> {
        if let Some(tx) = &self.tx {
            let _ = tx.send(Cmd::Audio(super::pcm_bytes(pcm)));
        }
        Ok(())
    }

    fn finish(&mut self) -> Result<(), AsrError> {
        if let Some(tx) = &self.tx {
            let _ = tx.send(Cmd::Finish);
        }
        Ok(())
    }

    fn cancel(&mut self) {
        self.cancel.store(true, Ordering::Release);
        self.tx = None;
    }
}

trait Stream: Read + Write + Send {}
impl<T: Read + Write + Send> Stream for T {}

/// TCP (+ TLS for `wss`) connection; returns the stream and a handle used to
/// change the read timeout after the handshake.
fn connect(request: &Request) -> Result<(Box<dyn Stream>, TcpStream), AsrError> {
    let uri = request.uri();
    let host = uri.host().ok_or_else(|| AsrError::new(AsrErrorKind::Protocol, "URL without host"))?.to_string();
    let tls = match uri.scheme_str() {
        Some("wss") => true,
        Some("ws") => false,
        _ => return Err(AsrError::new(AsrErrorKind::Protocol, "URL must be ws:// or wss://")),
    };
    let port = uri.port_u16().unwrap_or(if tls { 443 } else { 80 });
    let addr = (host.as_str(), port)
        .to_socket_addrs()
        .map_err(|e| AsrError::new(AsrErrorKind::Network, format!("resolve {host}: {e}")))?
        .next()
        .ok_or_else(|| AsrError::new(AsrErrorKind::Network, format!("resolve {host}: no address")))?;
    let tcp = TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT)
        .map_err(|e| AsrError::new(AsrErrorKind::Network, format!("connect {host}: {e}")))?;
    let io = |e: std::io::Error| AsrError::new(AsrErrorKind::Network, e.to_string());
    tcp.set_read_timeout(Some(HANDSHAKE_TIMEOUT)).map_err(io)?;
    tcp.set_write_timeout(Some(Duration::from_secs(5))).map_err(io)?;
    tcp.set_nodelay(true).map_err(io)?;
    let control = tcp.try_clone().map_err(io)?;
    if !tls {
        return Ok((Box::new(tcp), control));
    }
    #[cfg(target_os = "windows")]
    {
        let connector = native_tls::TlsConnector::new()
            .map_err(|e| AsrError::new(AsrErrorKind::Network, format!("TLS setup: {e}")))?;
        let stream = connector
            .connect(&host, tcp)
            .map_err(|e| AsrError::new(AsrErrorKind::Network, format!("TLS {host}: {e}")))?;
        Ok((Box::new(stream), control))
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = control;
        Err(AsrError::new(AsrErrorKind::Network, "TLS is only built for Windows"))
    }
}

fn worker<P: WireProtocol>(
    mut proto: P,
    request: Request,
    opts: &StartOptions,
    rx: Receiver<Cmd>,
    cancel: &AtomicBool,
    events: &EventSink,
) -> Result<(), AsrError> {
    let (stream, control) = connect(&request)?;
    let (mut ws, _response) = tungstenite::client(request, stream).map_err(|e| match e {
        HandshakeError::Failure(WsError::Http(resp)) => {
            let status = resp.status().as_u16();
            let body = resp.body().as_ref().map(|b| String::from_utf8_lossy(b).chars().take(200).collect::<String>());
            let kind = if matches!(status, 401 | 403) { AsrErrorKind::Auth } else { AsrErrorKind::Server };
            AsrError::new(kind, format!("handshake HTTP {status} {}", body.unwrap_or_default()))
        }
        other => AsrError::new(AsrErrorKind::Network, format!("handshake: {other}")),
    })?;
    control
        .set_read_timeout(Some(POLL))
        .map_err(|e| AsrError::new(AsrErrorKind::Network, e.to_string()))?;

    let send = |ws: &mut tungstenite::WebSocket<Box<dyn Stream>>, msgs: Vec<Message>| -> Result<(), AsrError> {
        for m in msgs {
            ws.send(m).map_err(|e| AsrError::new(AsrErrorKind::Network, format!("send: {e}")))?;
        }
        Ok(())
    };
    send(&mut ws, proto.open(opts))?;

    let begun = Instant::now();
    let mut pending: Vec<u8> = Vec::new();
    let mut finishing = false;
    let mut finish_sent: Option<Instant> = None;
    loop {
        if cancel.load(Ordering::Acquire) {
            let _ = ws.close(None);
            return Ok(());
        }
        loop {
            match rx.try_recv() {
                Ok(Cmd::Audio(bytes)) => pending.extend(bytes),
                Ok(Cmd::Finish) => finishing = true,
                Err(TryRecvError::Empty) => break,
                // The engine was dropped: nobody is listening any more.
                Err(TryRecvError::Disconnected) => {
                    let _ = ws.close(None);
                    return Ok(());
                }
            }
        }
        if proto.can_stream() {
            let chunk = proto.chunk_bytes().max(2);
            while pending.len() >= chunk {
                let rest = pending.split_off(chunk);
                let msgs = proto.audio(std::mem::replace(&mut pending, rest));
                send(&mut ws, msgs)?;
            }
            if finishing && finish_sent.is_none() {
                let msgs = proto.finish(std::mem::take(&mut pending));
                send(&mut ws, msgs)?;
                finish_sent = Some(Instant::now());
            }
        } else if begun.elapsed() > START_TIMEOUT {
            return Err(AsrError::new(AsrErrorKind::Timeout, "service did not start the task"));
        }
        match ws.read() {
            Ok(msg) => match proto.handle(msg)? {
                Some(WireEvent::Final(text)) => {
                    events(AsrEvent::Final(text));
                    let _ = ws.close(None);
                    let _ = ws.flush();
                    return Ok(());
                }
                Some(WireEvent::Partial(text)) => events(AsrEvent::Partial(text)),
                Some(WireEvent::Started) | None => {}
            },
            Err(WsError::Io(e)) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(WsError::ConnectionClosed | WsError::AlreadyClosed) => {
                return Err(AsrError::new(AsrErrorKind::Network, "service closed the connection"));
            }
            Err(e) => return Err(AsrError::new(AsrErrorKind::Network, format!("read: {e}"))),
        }
        if begun.elapsed() > SESSION_TIMEOUT {
            return Err(AsrError::new(AsrErrorKind::Timeout, "utterance too long"));
        }
    }
}

/// Handshake request with extra headers. Header values come from settings
/// and the keychain; a value that is not a valid header is refused here.
pub(crate) fn request_with_headers(url: &str, headers: &[(&'static str, String)]) -> Result<Request, AsrError> {
    let mut request = url
        .into_client_request()
        .map_err(|e| AsrError::new(AsrErrorKind::Protocol, format!("bad URL: {e}")))?;
    for (name, value) in headers {
        let value = HeaderValue::from_str(value)
            .map_err(|_| AsrError::new(AsrErrorKind::Protocol, format!("invalid {name} header value")))?;
        request.headers_mut().insert(*name, value);
    }
    Ok(request)
}

#[cfg(test)]
pub(crate) mod mock {
    //! A one-connection WebSocket server on 127.0.0.1 for engine tests.
    use std::net::TcpListener;
    use std::thread::JoinHandle;
    use tungstenite::handshake::server::{Request, Response};
    use tungstenite::Message;

    pub struct Seen {
        pub headers: Vec<(String, String)>,
        pub messages: Vec<Message>,
    }

    /// Serve one client: record its handshake headers and every message;
    /// `reply` maps each client message to the server's answers. The server
    /// stops after a reply `None` (close) or when the client goes away.
    pub fn serve(
        mut reply: impl FnMut(&Message) -> Option<Vec<Message>> + Send + 'static,
    ) -> (String, JoinHandle<Seen>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("ws://{}", listener.local_addr().unwrap());
        let handle = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut headers = Vec::new();
            let mut ws = tungstenite::accept_hdr(stream, |req: &Request, resp: Response| {
                headers = req
                    .headers()
                    .iter()
                    .map(|(k, v)| (k.as_str().to_string(), v.to_str().unwrap_or("").to_string()))
                    .collect();
                Ok(resp)
            })
            .unwrap();
            let mut messages = Vec::new();
            while let Ok(msg) = ws.read() {
                if msg.is_close() {
                    break;
                }
                let answer = reply(&msg);
                messages.push(msg);
                match answer {
                    Some(out) => {
                        for m in out {
                            let _ = ws.send(m);
                        }
                    }
                    None => {
                        let _ = ws.close(None);
                        break;
                    }
                }
            }
            Seen { headers, messages }
        });
        (url, handle)
    }

    /// Collect engine events until a Final or Error.
    pub fn collect(rx: &std::sync::mpsc::Receiver<super::AsrEvent>) -> Vec<super::AsrEvent> {
        let mut out = Vec::new();
        while let Ok(ev) = rx.recv_timeout(std::time::Duration::from_secs(5)) {
            let done = !matches!(ev, super::AsrEvent::Partial(_));
            out.push(ev);
            if done {
                break;
            }
        }
        out
    }
}
