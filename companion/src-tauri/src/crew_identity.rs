//! Who is listening on the Kiro Crew port (CP-09).
//!
//! The Companion hands `.local_secret` (and later the owner session token) to
//! whatever accepts `127.0.0.1:<port>`. Before every hand-over it asks the OS
//! which process owns the listening socket and accepts it only when:
//!
//! (a) the process belongs to the current user (Windows: token user SID;
//!     Linux: socket owner UID from `/proc/net/tcp`), and
//! (b) its executable is the Kiro Crew gateway: a file inside the Kiro install
//!     root (the parent of the Crew home, `~/.kiro` by default), under a
//!     top-level directory whose name starts with `crew` (`crew-python`,
//!     `crew-venv-0.12.2`, `crew/kirocrew-bin`, ...), named `kirocrew`,
//!     `python`, `python3`, `python3.N`, `pythonw` (on Windows each with
//!     `.exe`, compared case-insensitively; elsewhere exact).
//!     The gateway is a Python process started by the Crew launcher; the
//!     interpreter path is what identifies the install.
//!
//! Every listener that can accept a 127.0.0.1 connection on the port
//! (bound to 127.0.0.1 or 0.0.0.0) must pass. Anything unknown fails closed.
//!
//! shortcut: the check and the connect are two steps, so a listener that is
//! swapped in between is not caught; upgrade to checking the connected
//! socket's peer (GetExtendedTcpTable row of our own connection) if that
//! window matters.

use std::fmt;
use std::path::{Component, Path, PathBuf};

/// A user identity as the OS reports it: a SID string on Windows, a UID on Unix.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnerId(pub String);

/// One process listening on the port.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Listener {
    pub pid: u32,
    pub owner: OwnerId,
    pub exe: PathBuf,
}

/// The OS queries the check needs. Real implementation per platform below;
/// tests inject a mock.
pub trait ListenerQuery {
    /// Every listener that would accept a connection to 127.0.0.1:`port`.
    fn listeners(&self, port: u16) -> Result<Vec<Listener>, String>;
    fn current_user(&self) -> Result<OwnerId, String>;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IdentityError {
    /// The OS query failed or is not available on this platform.
    Query(String),
    NoListener,
    OwnerMismatch { pid: u32 },
    NotGateway { pid: u32, exe: PathBuf },
}

impl fmt::Display for IdentityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{IDENTITY_FAILED}: ")?;
        match self {
            IdentityError::Query(e) => write!(f, "cannot read the port owner ({e})"),
            IdentityError::NoListener => write!(f, "nothing is listening on the Crew port"),
            IdentityError::OwnerMismatch { pid } => write!(f, "process {pid} belongs to another user"),
            IdentityError::NotGateway { pid, exe } => {
                write!(f, "process {pid} is not the Kiro Crew gateway ({})", exe.display())
            }
        }
    }
}

/// Prefix of every identity failure, matched by the UI and the keyboard notice.
pub const IDENTITY_FAILED: &str = "Crew identity check failed";

/// Verify the listener(s) on `port`. Returns the gateway PID.
pub fn verify_gateway(q: &dyn ListenerQuery, port: u16, install_root: &Path) -> Result<u32, IdentityError> {
    let me = q.current_user().map_err(IdentityError::Query)?;
    let listeners = q.listeners(port).map_err(IdentityError::Query)?;
    if listeners.is_empty() {
        return Err(IdentityError::NoListener);
    }
    for l in &listeners {
        if l.owner != me {
            return Err(IdentityError::OwnerMismatch { pid: l.pid });
        }
        if !gateway_exe_allowed(&l.exe, install_root, cfg!(windows)) {
            return Err(IdentityError::NotGateway { pid: l.pid, exe: l.exe.clone() });
        }
    }
    Ok(listeners[0].pid)
}

const GATEWAY_NAMES: [&str; 4] = ["kirocrew", "python", "python3", "pythonw"];

/// Rule (b) above. `fold_case`: compare case-insensitively (Windows paths).
pub fn gateway_exe_allowed(exe: &Path, install_root: &Path, fold_case: bool) -> bool {
    let norm = |p: &Path| -> Option<Vec<String>> {
        let mut out = Vec::new();
        for c in p.components() {
            match c {
                Component::Normal(s) => {
                    let s = s.to_string_lossy();
                    out.push(if fold_case { s.to_lowercase() } else { s.into_owned() });
                }
                Component::Prefix(p) => {
                    let s = p.as_os_str().to_string_lossy();
                    out.push(if fold_case { s.to_lowercase() } else { s.into_owned() });
                }
                Component::RootDir | Component::CurDir => {}
                // A path with `..` is not a resolved image path: refuse.
                Component::ParentDir => return None,
            }
        }
        Some(out)
    };
    let (Some(exe_parts), Some(root_parts)) = (norm(exe), norm(install_root)) else {
        return false;
    };
    if root_parts.is_empty() || exe_parts.len() < root_parts.len() + 2 || exe_parts[..root_parts.len()] != root_parts[..] {
        return false;
    }
    // Parts are already lower-case when fold_case; otherwise compare exactly.
    let top = &exe_parts[root_parts.len()];
    if !top.starts_with("crew") {
        return false;
    }
    let name = exe_parts.last().cloned().unwrap_or_default();
    let stem = if fold_case { name.strip_suffix(".exe").unwrap_or(&name) } else { name.as_str() };
    if GATEWAY_NAMES.contains(&stem) {
        return true;
    }
    // python3.12 and similar.
    stem.strip_prefix("python3.").is_some_and(|v| !v.is_empty() && v.bytes().all(|b| b.is_ascii_digit()))
}

/// The platform query.
pub fn system_query() -> Box<dyn ListenerQuery + Send + Sync> {
    Box::new(SystemQuery)
}

struct SystemQuery;

// ─────────────────────────────── Linux ───────────────────────────────

#[cfg(target_os = "linux")]
impl ListenerQuery for SystemQuery {
    fn listeners(&self, port: u16) -> Result<Vec<Listener>, String> {
        let table = std::fs::read_to_string("/proc/net/tcp").map_err(|e| format!("/proc/net/tcp: {e}"))?;
        let sockets = linux::listening_sockets(&table, port);
        let mut out = Vec::new();
        for (uid, inode) in sockets {
            let pid = linux::pid_for_inode(&inode)
                .ok_or_else(|| format!("no process found for socket inode {inode}"))?;
            let exe = std::fs::read_link(format!("/proc/{pid}/exe")).map_err(|e| format!("/proc/{pid}/exe: {e}"))?;
            out.push(Listener { pid, owner: OwnerId(uid), exe });
        }
        Ok(out)
    }

    fn current_user(&self) -> Result<OwnerId, String> {
        Ok(OwnerId(crate::crew_identity::current_uid().to_string()))
    }
}

#[cfg(unix)]
pub fn current_uid() -> u32 {
    extern "C" {
        fn geteuid() -> u32;
    }
    // SAFETY: geteuid has no preconditions and cannot fail.
    unsafe { geteuid() }
}

#[cfg(any(target_os = "linux", test))]
pub mod linux {
    /// (uid, inode) of every LISTEN socket (state 0A) bound to 127.0.0.1 or
    /// 0.0.0.0 on `port`, from the text of `/proc/net/tcp`.
    pub fn listening_sockets(table: &str, port: u16) -> Vec<(String, String)> {
        let mut out = Vec::new();
        for line in table.lines().skip(1) {
            let f: Vec<&str> = line.split_whitespace().collect();
            if f.len() < 10 || f[3] != "0A" {
                continue;
            }
            let Some((addr, p)) = f[1].split_once(':') else { continue };
            if u16::from_str_radix(p, 16).ok() != Some(port) {
                continue;
            }
            // Little-endian hex of the IPv4 address.
            if addr != "0100007F" && addr != "00000000" {
                continue;
            }
            out.push((f[7].to_string(), f[9].to_string()));
        }
        out
    }

    #[cfg(target_os = "linux")]
    pub fn pid_for_inode(inode: &str) -> Option<u32> {
        let want = format!("socket:[{inode}]");
        for entry in std::fs::read_dir("/proc").ok()?.flatten() {
            let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else { continue };
            let Ok(fds) = std::fs::read_dir(entry.path().join("fd")) else { continue };
            for fd in fds.flatten() {
                if std::fs::read_link(fd.path()).is_ok_and(|t| t.to_string_lossy() == want) {
                    return Some(pid);
                }
            }
        }
        None
    }
}

// ─────────────────────────────── macOS ───────────────────────────────

#[cfg(target_os = "macos")]
impl ListenerQuery for SystemQuery {
    // shortcut: no macOS query yet (the macOS Companion lives on its own
    // branch); fail closed. Upgrade with libproc proc_pidfdinfo +
    // proc_pidpath + proc_pidinfo(PROC_PIDTBSDINFO) for the UID.
    fn listeners(&self, _port: u16) -> Result<Vec<Listener>, String> {
        Err("port owner lookup is not implemented on macOS yet".into())
    }
    fn current_user(&self) -> Result<OwnerId, String> {
        Ok(OwnerId(current_uid().to_string()))
    }
}

// ────────────────────────────── Windows ──────────────────────────────

#[cfg(windows)]
impl ListenerQuery for SystemQuery {
    fn listeners(&self, port: u16) -> Result<Vec<Listener>, String> {
        win::listeners(port)
    }
    fn current_user(&self) -> Result<OwnerId, String> {
        win::current_user()
    }
}

#[cfg(windows)]
mod win {
    use super::{Listener, OwnerId};
    use std::ffi::c_void;
    use std::path::PathBuf;

    type HANDLE = isize;
    type BOOL = i32;

    const AF_INET: u32 = 2;
    const TCP_TABLE_OWNER_PID_LISTENER: u32 = 3;
    const NO_ERROR: u32 = 0;
    const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    const TOKEN_QUERY: u32 = 0x0008;
    const TOKEN_USER_CLASS: u32 = 1;

    #[repr(C)]
    struct MibTcpRowOwnerPid {
        state: u32,
        local_addr: u32,
        local_port: u32,
        remote_addr: u32,
        remote_port: u32,
        owning_pid: u32,
    }

    #[repr(C)]
    struct SidAndAttributes {
        sid: *mut c_void,
        attributes: u32,
    }

    #[link(name = "iphlpapi")]
    extern "system" {
        fn GetExtendedTcpTable(
            table: *mut c_void,
            size: *mut u32,
            order: BOOL,
            af: u32,
            class: u32,
            reserved: u32,
        ) -> u32;
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(access: u32, inherit: BOOL, pid: u32) -> HANDLE;
        fn GetCurrentProcess() -> HANDLE;
        fn CloseHandle(h: HANDLE) -> BOOL;
        fn QueryFullProcessImageNameW(h: HANDLE, flags: u32, name: *mut u16, size: *mut u32) -> BOOL;
        fn LocalFree(mem: *mut c_void) -> *mut c_void;
    }

    #[link(name = "advapi32")]
    extern "system" {
        fn OpenProcessToken(process: HANDLE, access: u32, token: *mut HANDLE) -> BOOL;
        fn GetTokenInformation(token: HANDLE, class: u32, info: *mut c_void, len: u32, ret: *mut u32) -> BOOL;
        fn ConvertSidToStringSidW(sid: *mut c_void, out: *mut *mut u16) -> BOOL;
    }

    struct Handle(HANDLE);
    impl Drop for Handle {
        fn drop(&mut self) {
            if self.0 != 0 && self.0 != -1 {
                // SAFETY: a handle this module opened and still owns.
                unsafe { CloseHandle(self.0) };
            }
        }
    }

    pub fn listeners(port: u16) -> Result<Vec<Listener>, String> {
        let mut size: u32 = 0;
        // SAFETY: a null buffer with size 0 asks for the required size.
        let rc = unsafe {
            GetExtendedTcpTable(std::ptr::null_mut(), &mut size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
        };
        if rc != ERROR_INSUFFICIENT_BUFFER && rc != NO_ERROR {
            return Err(format!("GetExtendedTcpTable size: {rc}"));
        }
        // Extra room in case the table grows between the two calls; u32
        // storage keeps the rows aligned.
        let mut buf = vec![0u32; (size as usize / 4) + 64];
        size = (buf.len() * 4) as u32;
        // SAFETY: buf holds `size` writable bytes.
        let rc = unsafe {
            GetExtendedTcpTable(buf.as_mut_ptr().cast(), &mut size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
        };
        if rc != NO_ERROR {
            return Err(format!("GetExtendedTcpTable: {rc}"));
        }
        let count = buf[0] as usize;
        let row_words = std::mem::size_of::<MibTcpRowOwnerPid>() / 4;
        if 1 + count * row_words > buf.len() {
            return Err("GetExtendedTcpTable: truncated table".into());
        }
        let mut out = Vec::new();
        for i in 0..count {
            // SAFETY: bounds checked above; rows are plain u32 fields.
            let row = unsafe { &*(buf.as_ptr().add(1 + i * row_words) as *const MibTcpRowOwnerPid) };
            let row_port = u16::from_be((row.local_port & 0xFFFF) as u16);
            let addr = std::net::Ipv4Addr::from(row.local_addr.to_ne_bytes());
            if row_port != port || !(addr == std::net::Ipv4Addr::LOCALHOST || addr.is_unspecified()) {
                continue;
            }
            let pid = row.owning_pid;
            // SAFETY: plain query access to a PID from the table.
            let h = Handle(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) });
            if h.0 == 0 {
                return Err(format!("OpenProcess({pid}) refused"));
            }
            let exe = image_path(h.0).ok_or_else(|| format!("image path of {pid} unavailable"))?;
            let owner = process_user(h.0).ok_or_else(|| format!("owner of {pid} unavailable"))?;
            out.push(Listener { pid, owner, exe });
        }
        Ok(out)
    }

    pub fn current_user() -> Result<OwnerId, String> {
        // SAFETY: the pseudo handle of this process needs no closing.
        process_user(unsafe { GetCurrentProcess() }).ok_or_else(|| "current user SID unavailable".into())
    }

    fn image_path(h: HANDLE) -> Option<PathBuf> {
        let mut buf = vec![0u16; 1024];
        let mut len = buf.len() as u32;
        // SAFETY: buf holds `len` u16s.
        if unsafe { QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut len) } == 0 {
            return None;
        }
        Some(PathBuf::from(String::from_utf16_lossy(&buf[..len as usize])))
    }

    fn process_user(process: HANDLE) -> Option<OwnerId> {
        let mut tok: HANDLE = 0;
        // SAFETY: out-pointer to a local.
        if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut tok) } == 0 {
            return None;
        }
        let tok = Handle(tok);
        let mut need: u32 = 0;
        // SAFETY: size query with a null buffer.
        unsafe { GetTokenInformation(tok.0, TOKEN_USER_CLASS, std::ptr::null_mut(), 0, &mut need) };
        if need == 0 || need > 4096 {
            return None;
        }
        let mut buf = vec![0u64; (need as usize).div_ceil(8)];
        // SAFETY: buf holds at least `need` bytes, 8-byte aligned.
        if unsafe { GetTokenInformation(tok.0, TOKEN_USER_CLASS, buf.as_mut_ptr().cast(), need, &mut need) } == 0 {
            return None;
        }
        // SAFETY: TOKEN_USER starts with SID_AND_ATTRIBUTES.
        let sid = unsafe { (*(buf.as_ptr() as *const SidAndAttributes)).sid };
        let mut s: *mut u16 = std::ptr::null_mut();
        // SAFETY: sid points into buf; s receives a LocalAlloc'd string.
        if unsafe { ConvertSidToStringSidW(sid, &mut s) } == 0 || s.is_null() {
            return None;
        }
        let mut n = 0usize;
        // SAFETY: NUL-terminated wide string from ConvertSidToStringSidW.
        while unsafe { *s.add(n) } != 0 && n < 256 {
            n += 1;
        }
        // SAFETY: n u16s were just read.
        let text = String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(s, n) });
        // SAFETY: allocated by ConvertSidToStringSidW.
        unsafe { LocalFree(s.cast()) };
        Some(OwnerId(text))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Mock {
        me: &'static str,
        listeners: Result<Vec<Listener>, String>,
    }

    impl ListenerQuery for Mock {
        fn listeners(&self, _port: u16) -> Result<Vec<Listener>, String> {
            self.listeners.clone()
        }
        fn current_user(&self) -> Result<OwnerId, String> {
            Ok(OwnerId(self.me.into()))
        }
    }

    fn l(pid: u32, owner: &str, exe: &str) -> Listener {
        Listener { pid, owner: OwnerId(owner.into()), exe: PathBuf::from(exe) }
    }

    const ROOT: &str = "/home/u/.kiro";
    const GW: &str = "/home/u/.kiro/crew-python/cpython-3.12.13-linux-x86_64-gnu/bin/python3.12";

    #[test]
    fn genuine_gateway_passes() {
        let q = Mock { me: "1000", listeners: Ok(vec![l(42, "1000", GW)]) };
        assert_eq!(verify_gateway(&q, 5476, Path::new(ROOT)), Ok(42));
    }

    #[test]
    fn other_users_listener_fails_closed() {
        let q = Mock { me: "1000", listeners: Ok(vec![l(42, "0", GW)]) };
        assert_eq!(verify_gateway(&q, 5476, Path::new(ROOT)), Err(IdentityError::OwnerMismatch { pid: 42 }));
        // Every listener must pass: one impostor next to the gateway is refused.
        let q = Mock { me: "1000", listeners: Ok(vec![l(42, "1000", GW), l(7, "1001", GW)]) };
        assert_eq!(verify_gateway(&q, 5476, Path::new(ROOT)), Err(IdentityError::OwnerMismatch { pid: 7 }));
    }

    #[test]
    fn wrong_executable_fails_closed() {
        let q = Mock { me: "1000", listeners: Ok(vec![l(9, "1000", "/usr/bin/python3")]) };
        let err = verify_gateway(&q, 5476, Path::new(ROOT)).unwrap_err();
        assert!(matches!(err, IdentityError::NotGateway { pid: 9, .. }));
        assert!(err.to_string().starts_with(IDENTITY_FAILED));
        let q = Mock { me: "1000", listeners: Ok(vec![l(9, "1000", "/home/u/.kiro/crew-python/bin/node")]) };
        assert!(matches!(verify_gateway(&q, 5476, Path::new(ROOT)), Err(IdentityError::NotGateway { .. })));
    }

    #[test]
    fn query_failure_and_no_listener_fail_closed() {
        let q = Mock { me: "1000", listeners: Err("denied".into()) };
        assert_eq!(verify_gateway(&q, 5476, Path::new(ROOT)), Err(IdentityError::Query("denied".into())));
        let q = Mock { me: "1000", listeners: Ok(vec![]) };
        assert_eq!(verify_gateway(&q, 5476, Path::new(ROOT)), Err(IdentityError::NoListener));
    }

    #[test]
    fn executable_rule() {
        let root = Path::new(ROOT);
        for ok in [
            GW,
            "/home/u/.kiro/crew-venv-0.12.2/bin/python3",
            "/home/u/.kiro/crew/kirocrew-bin/kirocrew",
        ] {
            assert!(gateway_exe_allowed(Path::new(ok), root, false), "{ok}");
        }
        for bad in [
            "/home/u/.kiro/crew-python/python3.x",
            "/home/u/.kiro/other/bin/python3",
            "/home/u/.kiro/python3",
            "/home/u/.kiro/crew-python/../../evil/python3",
            "/home/u/.kirox/crew-python/bin/python3",
            "/tmp/crew-python/bin/python3",
            "/home/u/.kiro/crew-python/bin/Python3",
            "/home/u/.kiro/crew-python/bin/python3.exe",
        ] {
            assert!(!gateway_exe_allowed(Path::new(bad), root, false), "{bad}");
        }
        // Windows paths compare case-insensitively.
        assert!(gateway_exe_allowed(
            Path::new("/Users/U/.Kiro/Crew-Python/Python.EXE"),
            Path::new("/users/u/.kiro"),
            true
        ));
        assert!(gateway_exe_allowed(Path::new("/users/u/.kiro/crew/kirocrew.exe"), Path::new("/Users/U/.kiro"), true));
    }

    #[test]
    fn proc_net_tcp_parsing() {
        let table = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1564 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 111 1 0
   1: 00000000:1564 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1001        0 222 1 0
   2: 0100007F:1564 0100007F:9C40 01 00000000:00000000 00:00000000 00000000  1000        0 333 1 0
   3: 0A00000B:1564 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 444 1 0
   4: 0100007F:1565 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 555 1 0";
        assert_eq!(
            linux::listening_sockets(table, 5476),
            vec![("1000".to_string(), "111".to_string()), ("1001".to_string(), "222".to_string())]
        );
    }
}
