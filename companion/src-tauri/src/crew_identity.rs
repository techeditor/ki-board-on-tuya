//! Who is listening on the Kiro Crew port (CP-09).
//!
//! The Companion hands `.local_secret` (and later the owner session token) to
//! whatever accepts `127.0.0.1:<port>`. Before every hand-over it asks the OS
//! which process owns the listening socket and accepts it only when:
//!
//! (a) the process belongs to the current user (Windows: token user SID;
//!     Linux: socket owner UID from `/proc/net/tcp`), and
//! (b) its executable is the Kiro Crew gateway: a file inside the Kiro Crew install
//!     root (the parent of the Crew home, `~/.kiro` by default), under a
//!     top-level directory whose name starts with `crew` (`crew-python`,
//!     `crew-venv-0.12.2`, `crew/kirocrew-bin`, ...), named `kirocrew`,
//!     `python`, `python3`, `python3.N`, `pythonw` (on Windows each with
//!     `.exe`, compared case-insensitively; elsewhere exact).
//!     The gateway is a Python process started by the Crew launcher; the
//!     interpreter path is what identifies the install.
//!     Or the desktop app's bundled interpreter: `<app>\resources\backend-dist\
//!     kirocrew-backend*\[bin\]python.exe` (any name above), where `<app>` is
//!     a Kiro Crew install directory (Windows: `%LOCALAPPDATA%\Programs\
//!     KiroCrew` and every `InstallLocation` of a "Kiro Crew" uninstall entry
//!     in the registry). The desktop app must vouch for it too: the gateway's
//!     first ancestor outside the bundle runs from under `<app>` (the app
//!     spawned it; Crew's own `cli_server_stop._desktop_supervisor_pid` reads
//!     the same rule), or, when that ancestor is gone (the app quit), `<app>`
//!     still holds the app's own `*kiro*.exe`.
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
    /// Image paths of `pid`'s parent, grandparent, ... (at most
    /// [`ANCESTOR_DEPTH`]); stops at the first one that cannot be read.
    fn ancestor_exes(&self, _pid: u32) -> Vec<PathBuf> {
        Vec::new()
    }
    /// File names directly in `dir` (empty when unreadable).
    fn file_names(&self, dir: &Path) -> Vec<String> {
        std::fs::read_dir(dir)
            .map(|rd| {
                rd.flatten()
                    .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .collect()
            })
            .unwrap_or_default()
    }
}

/// How many ancestors of the gateway are read to find the desktop app (the
/// app spawns the interpreter directly; slack for a launcher shim).
pub const ANCESTOR_DEPTH: usize = 4;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IdentityError {
    /// The OS query failed or is not available on this platform.
    Query(String),
    NoListener,
    OwnerMismatch { pid: u32 },
    NotGateway { pid: u32, exe: PathBuf, reason: String },
}

impl fmt::Display for IdentityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{IDENTITY_FAILED}: ")?;
        match self {
            IdentityError::Query(e) => write!(f, "cannot read the port owner ({e})"),
            IdentityError::NoListener => write!(f, "nothing is listening on the Crew port"),
            IdentityError::OwnerMismatch { pid } => write!(f, "process {pid} belongs to another user"),
            IdentityError::NotGateway { pid, exe, reason } => {
                write!(f, "process {pid} is not the Kiro Crew gateway ({}): {reason}", exe.display())
            }
        }
    }
}

/// Prefix of every identity failure, matched by the UI and the keyboard notice.
pub const IDENTITY_FAILED: &str = "Crew identity check failed";

/// Where a gateway may run from: the Kiro Crew install root (`~/.kiro`) and the
/// desktop app's install directories.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GatewayRoots {
    pub install_root: PathBuf,
    pub app_roots: Vec<PathBuf>,
}

/// Verify the listener(s) on `port`. Returns the gateway PID.
pub fn verify_gateway(q: &dyn ListenerQuery, port: u16, roots: &GatewayRoots) -> Result<u32, IdentityError> {
    verify_gateway_with(q, port, roots, cfg!(windows))
}

/// [`verify_gateway`] with the path case rule explicit (`fold`: Windows).
pub fn verify_gateway_with(
    q: &dyn ListenerQuery,
    port: u16,
    roots: &GatewayRoots,
    fold: bool,
) -> Result<u32, IdentityError> {
    let me = q.current_user().map_err(IdentityError::Query)?;
    let listeners = q.listeners(port).map_err(IdentityError::Query)?;
    if listeners.is_empty() {
        return Err(IdentityError::NoListener);
    }
    for l in &listeners {
        if l.owner != me {
            return Err(IdentityError::OwnerMismatch { pid: l.pid });
        }
        let not = |reason: String| IdentityError::NotGateway { pid: l.pid, exe: l.exe.clone(), reason };
        if gateway_exe_allowed(&l.exe, &roots.install_root, fold) {
            continue;
        }
        let app = match bundled_app_root(&l.exe, &roots.app_roots, fold) {
            Ok(app) => app,
            Err(why) => return Err(not(why)),
        };
        desktop_vouches(q, l.pid, &l.exe, &app, fold).map_err(not)?;
    }
    Ok(listeners[0].pid)
}

/// The path's components, lower-cased when `fold_case`; `None` for a path
/// with `..` (not a resolved image path).
fn parts(p: &Path, fold_case: bool) -> Option<Vec<String>> {
    let mut out = Vec::new();
    for c in p.components() {
        let s = match c {
            Component::Normal(s) => s.to_string_lossy().into_owned(),
            Component::Prefix(p) => p.as_os_str().to_string_lossy().into_owned(),
            Component::RootDir | Component::CurDir => continue,
            Component::ParentDir => return None,
        };
        out.push(if fold_case { s.to_lowercase() } else { s });
    }
    Some(out)
}

fn under(path: &[String], root: &[String]) -> bool {
    !root.is_empty() && path.len() > root.len() && path[..root.len()] == root[..]
}

/// An allowed interpreter / launcher file name (python3.12 and similar too).
fn gateway_name(name: &str, fold_case: bool) -> bool {
    let stem = if fold_case { name.strip_suffix(".exe").unwrap_or(name) } else { name };
    GATEWAY_NAMES.contains(&stem)
        || stem.strip_prefix("python3.").is_some_and(|v| !v.is_empty() && v.bytes().all(|b| b.is_ascii_digit()))
}

/// The desktop install directory `exe` is the bundled gateway of:
/// `<app>/resources/backend-dist/kirocrew-backend*/[bin/]<gateway name>`.
/// Err names why it is not.
pub fn bundled_app_root(exe: &Path, app_roots: &[PathBuf], fold_case: bool) -> Result<PathBuf, String> {
    let e = parts(exe, fold_case).ok_or("the path contains '..'")?;
    let name = e.last().cloned().unwrap_or_default();
    if !gateway_name(&name, fold_case) {
        return Err(format!("'{name}' is not a Kiro Crew interpreter or launcher name"));
    }
    if app_roots.is_empty() {
        return Err("outside the Kiro Crew install root, and no Kiro Crew desktop install was found".into());
    }
    for root in app_roots {
        let Some(r) = parts(root, fold_case) else { continue };
        if !under(&e, &r) {
            continue;
        }
        let rest = &e[r.len()..e.len() - 1];
        let shape_ok = (rest.len() == 3 || (rest.len() == 4 && rest[3] == "bin"))
            && rest[0] == "resources"
            && rest[1] == "backend-dist"
            && rest[2].starts_with("kirocrew-backend");
        return if shape_ok {
            Ok(root.clone())
        } else {
            Err(format!(
                "inside the Kiro Crew app {} but not in resources{sep}backend-dist{sep}kirocrew-backend",
                root.display(),
                sep = std::path::MAIN_SEPARATOR
            ))
        };
    }
    let listed: Vec<String> = app_roots.iter().map(|r| r.display().to_string()).collect();
    Err(format!("outside the Kiro Crew install root and the Kiro Crew app ({})", listed.join("; ")))
}

/// The desktop app vouches for its bundled gateway: the first ancestor
/// outside the bundle runs from under `app`, or (no such ancestor readable:
/// the app quit) `app` still holds the app's `*kiro*.exe`.
fn desktop_vouches(q: &dyn ListenerQuery, pid: u32, exe: &Path, app: &Path, fold_case: bool) -> Result<(), String> {
    let (Some(e), Some(a)) = (parts(exe, fold_case), parts(app, fold_case)) else {
        return Err("unreadable path".into());
    };
    // `<app>/resources/backend-dist/kirocrew-backend*`
    let bundle = &e[..a.len() + 3];
    for anc in q.ancestor_exes(pid) {
        let Some(p) = parts(&anc, fold_case) else { continue };
        if under(&p, bundle) {
            continue; // a launcher shim inside the bundle
        }
        return if under(&p, &a) {
            Ok(())
        } else {
            Err(format!("started by {}, not by the Kiro Crew app", anc.display()))
        };
    }
    let has_app = q.file_names(app).iter().any(|n| {
        let n = n.to_lowercase();
        n.ends_with(".exe") && n.contains("kiro") && !n.starts_with("uninstall")
    });
    if has_app {
        Ok(())
    } else {
        Err(format!("no Kiro Crew app executable in {}", app.display()))
    }
}

const GATEWAY_NAMES: [&str; 4] = ["kirocrew", "python", "python3", "pythonw"];

/// Rule (b) above for the Crew home. `fold_case`: compare case-insensitively
/// (Windows paths).
pub fn gateway_exe_allowed(exe: &Path, install_root: &Path, fold_case: bool) -> bool {
    let (Some(exe_parts), Some(root_parts)) = (parts(exe, fold_case), parts(install_root, fold_case)) else {
        return false;
    };
    if root_parts.is_empty() || exe_parts.len() < root_parts.len() + 2 || exe_parts[..root_parts.len()] != root_parts[..] {
        return false;
    }
    // Parts are already lower-case when fold_case; otherwise compare exactly.
    if !exe_parts[root_parts.len()].starts_with("crew") {
        return false;
    }
    gateway_name(exe_parts.last().map(String::as_str).unwrap_or(""), fold_case)
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
    fn ancestor_exes(&self, pid: u32) -> Vec<PathBuf> {
        win::ancestor_exes(pid)
    }
}

/// The Kiro Crew desktop app's install directories (Windows; empty
/// elsewhere): the per-user default `%LOCALAPPDATA%\Programs\KiroCrew`, and
/// the `InstallLocation` of every uninstall entry named "Kiro Crew" /
/// "KiroCrew" (per-user and per-machine).
pub fn desktop_app_roots() -> Vec<PathBuf> {
    #[cfg(windows)]
    {
        let mut out = Vec::new();
        if let Some(local) = std::env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()) {
            out.push(PathBuf::from(local).join("Programs").join("KiroCrew"));
        }
        for p in win::registry_install_locations() {
            if !out.iter().any(|o: &PathBuf| o.as_os_str().eq_ignore_ascii_case(p.as_os_str())) {
                out.push(p);
            }
        }
        out
    }
    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

/// Whether an uninstall entry's DisplayName is the Kiro Crew desktop app.
pub fn is_crew_display_name(name: &str) -> bool {
    let n: String = name.to_lowercase().chars().filter(|c| !c.is_whitespace()).collect();
    n.starts_with("kirocrew")
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

    const TH32CS_SNAPPROCESS: u32 = 0x2;

    #[repr(C)]
    struct ProcessEntry32W {
        size: u32,
        usage: u32,
        pid: u32,
        default_heap_id: usize,
        module_id: u32,
        threads: u32,
        parent_pid: u32,
        pri_class_base: i32,
        flags: u32,
        exe_file: [u16; 260],
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateToolhelp32Snapshot(flags: u32, pid: u32) -> HANDLE;
        fn Process32FirstW(snap: HANDLE, entry: *mut ProcessEntry32W) -> BOOL;
        fn Process32NextW(snap: HANDLE, entry: *mut ProcessEntry32W) -> BOOL;
    }

    /// pid -> parent pid for every process, from one snapshot.
    fn parent_table() -> Vec<(u32, u32)> {
        // SAFETY: plain snapshot of the process list.
        let snap = Handle(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) });
        if snap.0 == 0 || snap.0 == -1 {
            return Vec::new();
        }
        let mut out = Vec::new();
        // SAFETY: zeroed POD with its size set, as the API requires.
        let mut e: ProcessEntry32W = unsafe { std::mem::zeroed() };
        e.size = std::mem::size_of::<ProcessEntry32W>() as u32;
        // SAFETY: e is a valid, sized entry.
        let mut ok = unsafe { Process32FirstW(snap.0, &mut e) } != 0;
        while ok {
            out.push((e.pid, e.parent_pid));
            // SAFETY: as above.
            ok = unsafe { Process32NextW(snap.0, &mut e) } != 0;
        }
        out
    }

    /// Image paths of `pid`'s ancestors, nearest first.
    /// shortcut: a parent pid can be reused after the parent exits; the rule
    /// then sees an unrelated process and refuses (or, for a process under the
    /// app root, accepts like the file fallback would). Compare creation times
    /// if that matters.
    pub fn ancestor_exes(pid: u32) -> Vec<PathBuf> {
        let table = parent_table();
        let mut out = Vec::new();
        let mut cur = pid;
        for _ in 0..super::ANCESTOR_DEPTH {
            let Some(&(_, parent)) = table.iter().find(|(p, _)| *p == cur) else { break };
            if parent == 0 || parent == cur {
                break;
            }
            // SAFETY: plain query access to a PID from the snapshot.
            let h = Handle(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, parent) });
            let Some(exe) = (h.0 != 0).then(|| image_path(h.0)).flatten() else { break };
            out.push(exe);
            cur = parent;
        }
        out
    }

    type HKEY = isize;
    const HKEY_CURRENT_USER: HKEY = 0x8000_0001_u32 as i32 as isize;
    const HKEY_LOCAL_MACHINE: HKEY = 0x8000_0002_u32 as i32 as isize;
    const KEY_READ: u32 = 0x20019;
    const RRF_RT_REG_SZ: u32 = 0x2;
    const UNINSTALL: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall";

    #[link(name = "advapi32")]
    extern "system" {
        fn RegOpenKeyExW(key: HKEY, sub: *const u16, opts: u32, sam: u32, out: *mut HKEY) -> i32;
        fn RegEnumKeyExW(
            key: HKEY,
            index: u32,
            name: *mut u16,
            name_len: *mut u32,
            reserved: *mut u32,
            class: *mut u16,
            class_len: *mut u32,
            time: *mut u64,
        ) -> i32;
        fn RegGetValueW(
            key: HKEY,
            sub: *const u16,
            value: *const u16,
            flags: u32,
            kind: *mut u32,
            data: *mut c_void,
            len: *mut u32,
        ) -> i32;
        fn RegCloseKey(key: HKEY) -> i32;
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn reg_string(key: HKEY, sub: &[u16], value: &str) -> Option<String> {
        let value = wide(value);
        let mut buf = vec![0u16; 1024];
        let mut len = (buf.len() * 2) as u32;
        // SAFETY: buf holds `len` bytes; strings are NUL-terminated.
        let rc = unsafe {
            RegGetValueW(key, sub.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ, std::ptr::null_mut(), buf.as_mut_ptr().cast(), &mut len)
        };
        if rc != 0 {
            return None;
        }
        let n = (len as usize / 2).min(buf.len());
        let s = String::from_utf16_lossy(&buf[..n]);
        Some(s.trim_end_matches('\0').trim().trim_matches('"').to_string())
    }

    /// `InstallLocation` of each "Kiro Crew" uninstall entry.
    pub fn registry_install_locations() -> Vec<PathBuf> {
        let mut out = Vec::new();
        for hive in [HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE] {
            let mut key: HKEY = 0;
            let path = wide(UNINSTALL);
            // SAFETY: out-pointer to a local; read-only access.
            if unsafe { RegOpenKeyExW(hive, path.as_ptr(), 0, KEY_READ, &mut key) } != 0 {
                continue;
            }
            for i in 0..4096u32 {
                let mut name = vec![0u16; 256];
                let mut len = name.len() as u32;
                // SAFETY: name holds `len` u16s.
                let rc = unsafe {
                    RegEnumKeyExW(key, i, name.as_mut_ptr(), &mut len, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut())
                };
                if rc != 0 {
                    break;
                }
                let mut sub = name[..len as usize].to_vec();
                sub.push(0);
                let shown = reg_string(key, &sub, "DisplayName").unwrap_or_default();
                if !super::is_crew_display_name(&shown) {
                    continue;
                }
                if let Some(loc) = reg_string(key, &sub, "InstallLocation").filter(|l| !l.is_empty()) {
                    out.push(PathBuf::from(loc));
                }
            }
            // SAFETY: a key this function opened.
            unsafe { RegCloseKey(key) };
        }
        out
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
        ancestors: Vec<&'static str>,
        files: Vec<&'static str>,
    }

    impl Mock {
        fn new(me: &'static str, listeners: Result<Vec<Listener>, String>) -> Self {
            Mock { me, listeners, ancestors: vec![], files: vec![] }
        }
    }

    impl ListenerQuery for Mock {
        fn listeners(&self, _port: u16) -> Result<Vec<Listener>, String> {
            self.listeners.clone()
        }
        fn current_user(&self) -> Result<OwnerId, String> {
            Ok(OwnerId(self.me.into()))
        }
        fn ancestor_exes(&self, _pid: u32) -> Vec<PathBuf> {
            self.ancestors.iter().map(PathBuf::from).collect()
        }
        fn file_names(&self, _dir: &Path) -> Vec<String> {
            self.files.iter().map(|f| f.to_string()).collect()
        }
    }

    fn roots() -> GatewayRoots {
        GatewayRoots { install_root: PathBuf::from(ROOT), app_roots: vec![] }
    }

    fn l(pid: u32, owner: &str, exe: &str) -> Listener {
        Listener { pid, owner: OwnerId(owner.into()), exe: PathBuf::from(exe) }
    }

    const ROOT: &str = "/home/u/.kiro";
    const GW: &str = "/home/u/.kiro/crew-python/cpython-3.12.13-linux-x86_64-gnu/bin/python3.12";

    #[test]
    fn genuine_gateway_passes() {
        let q = Mock::new("1000", Ok(vec![l(42, "1000", GW)]));
        assert_eq!(verify_gateway(&q, 5476, &roots()), Ok(42));
    }

    #[test]
    fn other_users_listener_fails_closed() {
        let q = Mock::new("1000", Ok(vec![l(42, "0", GW)]));
        assert_eq!(verify_gateway(&q, 5476, &roots()), Err(IdentityError::OwnerMismatch { pid: 42 }));
        // Every listener must pass: one impostor next to the gateway is refused.
        let q = Mock::new("1000", Ok(vec![l(42, "1000", GW), l(7, "1001", GW)]));
        assert_eq!(verify_gateway(&q, 5476, &roots()), Err(IdentityError::OwnerMismatch { pid: 7 }));
    }

    #[test]
    fn wrong_executable_fails_closed() {
        let q = Mock::new("1000", Ok(vec![l(9, "1000", "/usr/bin/python3")]));
        let err = verify_gateway(&q, 5476, &roots()).unwrap_err();
        assert!(matches!(err, IdentityError::NotGateway { pid: 9, .. }));
        assert!(err.to_string().starts_with(IDENTITY_FAILED));
        let q = Mock::new("1000", Ok(vec![l(9, "1000", "/home/u/.kiro/crew-python/bin/node")]));
        assert!(matches!(verify_gateway(&q, 5476, &roots()), Err(IdentityError::NotGateway { .. })));
    }

    #[test]
    fn query_failure_and_no_listener_fail_closed() {
        let q = Mock::new("1000", Err("denied".into()));
        assert_eq!(verify_gateway(&q, 5476, &roots()), Err(IdentityError::Query("denied".into())));
        let q = Mock::new("1000", Ok(vec![]));
        assert_eq!(verify_gateway(&q, 5476, &roots()), Err(IdentityError::NoListener));
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

    // The Windows desktop install (user report, Companion 0.3.9): the gateway
    // is the app's bundled python.exe. Paths written with `/` so the test runs
    // on every host; Windows folds case.
    const APP: &str = "/C:/Users/U/AppData/Local/Programs/KiroCrew";
    const BUNDLED: &str =
        "/C:/Users/U/AppData/Local/Programs/KiroCrew/resources/backend-dist/kirocrew-backend/python.exe";

    fn desktop() -> GatewayRoots {
        GatewayRoots { install_root: PathBuf::from("/C:/Users/U/.kiro"), app_roots: vec![PathBuf::from(APP)] }
    }

    #[test]
    fn desktop_bundled_gateway_passes() {
        // Spawned by the app.
        let mut q = Mock::new("S-1-5-21-1", Ok(vec![l(20240, "S-1-5-21-1", BUNDLED)]));
        q.ancestors = vec!["/c:/users/u/appdata/local/programs/kirocrew/Kiro Crew.exe"];
        assert_eq!(verify_gateway_with(&q, 5476, &desktop(), true), Ok(20240));
        // A launcher shim inside the bundle is skipped on the way up.
        q.ancestors = vec![
            "/C:/Users/U/AppData/Local/Programs/KiroCrew/resources/backend-dist/kirocrew-backend/kirocrew.exe",
            "/C:/Users/U/AppData/Local/Programs/KiroCrew/KiroCrew.exe",
        ];
        assert_eq!(verify_gateway_with(&q, 5476, &desktop(), true), Ok(20240));
        // The app quit: its executable is still in the install directory.
        q.ancestors = vec![];
        q.files = vec!["Uninstall Kiro Crew.exe", "Kiro Crew.exe", "ffmpeg.dll"];
        assert_eq!(verify_gateway_with(&q, 5476, &desktop(), true), Ok(20240));
        // bin/ layout and pythonw too.
        let alt = "/C:/Users/U/AppData/Local/Programs/KiroCrew/resources/backend-dist/kirocrew-backend-x64/bin/pythonw.exe";
        let q = Mock { ancestors: vec!["/C:/Users/U/AppData/Local/Programs/KiroCrew/Kiro Crew.exe"], ..Mock::new("S", Ok(vec![l(1, "S", alt)])) };
        assert_eq!(verify_gateway_with(&q, 5476, &desktop(), true), Ok(1));
    }

    #[test]
    fn other_pythons_fail_closed_with_a_reason() {
        let reason = |q: &Mock| match verify_gateway_with(q, 5476, &desktop(), true) {
            Err(IdentityError::NotGateway { reason, .. }) => reason,
            other => panic!("expected NotGateway, got {other:?}"),
        };
        // Another directory.
        let q = Mock::new("S", Ok(vec![l(5, "S", "/C:/Python312/python.exe")]));
        assert!(reason(&q).contains("outside the Kiro Crew install root and the Kiro Crew app"), "{}", reason(&q));
        // Inside the app, but not the bundle.
        let q = Mock::new("S", Ok(vec![l(5, "S", "/C:/Users/U/AppData/Local/Programs/KiroCrew/python.exe")]));
        assert!(reason(&q).contains("not in resources"), "{}", reason(&q));
        let q = Mock::new("S", Ok(vec![l(5, "S", "/C:/Users/U/AppData/Local/Programs/KiroCrew/resources/backend-dist/other/python.exe")]));
        assert!(reason(&q).contains("not in resources"));
        // Not an interpreter name.
        let q = Mock::new("S", Ok(vec![l(5, "S", "/C:/Users/U/AppData/Local/Programs/KiroCrew/resources/backend-dist/kirocrew-backend/node.exe")]));
        assert!(reason(&q).contains("'node.exe'"));
        // A python copied into the bundle but started by something else.
        let mut q = Mock::new("S", Ok(vec![l(5, "S", BUNDLED)]));
        q.ancestors = vec!["/C:/Windows/System32/cmd.exe"];
        q.files = vec!["Kiro Crew.exe"];
        assert!(reason(&q).contains("started by"), "{}", reason(&q));
        // Parent gone and no app executable next to it.
        q.ancestors = vec![];
        q.files = vec!["Uninstall Kiro Crew.exe", "evil.exe"];
        assert!(reason(&q).contains("no Kiro Crew app executable"));
        // No desktop install known.
        let q = Mock::new("S", Ok(vec![l(5, "S", BUNDLED)]));
        let none = GatewayRoots { install_root: PathBuf::from("/C:/Users/U/.kiro"), app_roots: vec![] };
        match verify_gateway_with(&q, 5476, &none, true) {
            Err(IdentityError::NotGateway { reason, .. }) => assert!(reason.contains("no Kiro Crew desktop install")),
            other => panic!("{other:?}"),
        }
        // The message the log shows carries the reason.
        let q = Mock::new("S", Ok(vec![l(5, "S", "/C:/Python312/python.exe")]));
        let msg = verify_gateway_with(&q, 5476, &desktop(), true).unwrap_err().to_string();
        assert!(msg.starts_with(IDENTITY_FAILED) && msg.contains("): outside"), "{msg}");
    }

    #[test]
    fn bundled_gateway_of_another_user_fails_closed() {
        let mut q = Mock::new("S-1-5-21-1", Ok(vec![l(20240, "S-1-5-21-2", BUNDLED)]));
        q.ancestors = vec!["/C:/Users/U/AppData/Local/Programs/KiroCrew/Kiro Crew.exe"];
        assert_eq!(verify_gateway_with(&q, 5476, &desktop(), true), Err(IdentityError::OwnerMismatch { pid: 20240 }));
    }

    #[test]
    fn uninstall_entry_names() {
        assert!(is_crew_display_name("Kiro Crew 0.12.4"));
        assert!(is_crew_display_name("KiroCrew"));
        assert!(!is_crew_display_name("Kiro"));
        assert!(!is_crew_display_name("My Kiro Crew helper"));
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
