//! Companion app library entry.
//!
//! Skeleton: exposes the pure `protocol` and `transport` modules (unit-tested)
//! and a minimal Tauri `run()` that opens an empty window. Event routing,
//! keyboard injection, and ASR are added in Phase 1.

pub mod protocol;
pub mod transport;

/// Launch the Windows Tauri application (empty skeleton window).
#[cfg(target_os = "windows")]
pub fn run() {
    tauri::Builder::default()
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

/// Keep the pure Rust core testable on the Linux build host without GTK/WebKit.
#[cfg(not(target_os = "windows"))]
pub fn run() {
    eprintln!("the Companion desktop application is supported on Windows");
}
