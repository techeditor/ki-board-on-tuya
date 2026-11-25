fn main() {
    // Cargo runs this script on the build host, so `cfg!(target_os)` would describe the host.
    // Check the build target instead; that also covers cross compiling to Windows on Linux.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        tauri_build::build()
    }
}
