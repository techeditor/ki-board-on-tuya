# Contributing to Ki-board

Thank you for your interest in Ki-board, an AI keyboard for Kiro Crew users.
It moves Kiro Crew agent control from the screen to your fingertips: a fast
prompt-input keyboard and a physical entry point for driving agents. Kiro Crew
is the only supported agent today.

Bug reports, feature ideas, fixes, translations and documentation
improvements are all welcome. Please read this page before you open an issue
or a pull request. Issues and pull requests can be written in English or Chinese.

*(中文版见 [CONTRIBUTING.zh-CN.md](CONTRIBUTING.zh-CN.md))*

## Reporting bugs and requesting features

Use the GitHub issue tracker. Search open and recently closed issues first to
see whether somebody has reported the same thing. A useful report has:

- the firmware version (`firmware/app/VERSION`) and Companion version (`companion/VERSION`);
- your board, host OS and how the keyboard is connected (USB or Bluetooth);
- steps that reproduce the problem, and what you expected to happen;
- logs, with any Tuya product ID (PID), UUID, AuthKey, Wi-Fi password or pairing code removed.

**Security problems are not reported as issues**: see [Security](#security).

## Contributing via pull requests

1. **Open an issue first** for anything larger than a typo or a small fix, so we can agree on the approach before you spend time on it.
2. **Fork** the repository and create a branch from the latest `main`.
   Name it `<type>/<short-topic>` with a full English word as the type:
   `feature/`, `fix/`, `docs/`, `refactor/`, `test/` or `build/`
   (for example `feature/macos-key-injection`, `fix/ble-pairing-timeout`).
3. **Make one focused change.** Do not reformat unrelated code. Do not change
   version numbers (`VERSION` files and the version fields of manifests); the
   maintainers set them.
4. **Build and test** the part you changed (see [Build and test](#build-and-test)),
   and say in the pull request what you ran and on which board or OS.
5. **Keep your branch current by rebasing** onto `main`. Do not merge `main`
   into your branch, and do not leave merge commits. Your commits are applied
   as they are, so each one should be a meaningful, self-contained change.
6. **Open the pull request against `main`**, fill in the description (what, why,
   how you tested), and stay in the conversation until it is resolved.

### Pull request checklist

- [ ] It is linked to an issue (for anything beyond a small fix).
- [ ] It is rebased on the latest `main`, with no merge commits.
- [ ] Commit messages follow [Commits](#commits).
- [ ] The relevant build and tests pass, and the pull request says what you ran.
- [ ] No credentials, PID, UUID, AuthKey or firmware binaries (see [What never goes into git](#what-never-goes-into-git)).
- [ ] A change to the wire protocol updates the firmware, the Companion and `shared/protocol/protocol.md` together.
- [ ] Documentation is updated when behaviour changes.
- [ ] New third-party code, fonts or images have a compatible licence (see [Licensing](#licensing)).
- [ ] Version numbers are untouched.

### What happens to your pull request

This GitHub repository is a published snapshot of the maintainers' source
repository; it is not where changes are merged. After review, a maintainer
applies your commits in the source repository with you as their author, and the
change reaches this repository with the next published snapshot. The snapshot's
commit message credits you. Your pull request is then closed with a comment that
points to the snapshot containing your change. It is closed, not merged with the
GitHub button, because the snapshot replaces the branch history. We aim to
reply to every pull request, but we cannot promise a response time.

## Build and test

Run the checks for the part you changed. The repository has no automated
checks on pull requests yet, so these local runs are the checks.

**Companion** (Node 22.x, see `companion/.node-version`; Rust toolchain pinned in `rust-toolchain.toml`):

```bash
cd companion
npm ci
npm run typecheck                     # tsc --noEmit
npm run frontend:build                # vite build
cd src-tauri && cargo check           # Rust build check
```

`npm run tauri dev` starts the app and `npm run tauri build` builds the installer.

**Firmware** (needs a [TuyaOpen](https://github.com/tuya/TuyaOpen) checkout):

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config   # release profile
tos.py clean -f && tos.py build       # clean is required after any .config change
```

There are two profiles. `TUYA_T5AI_CORE_dual_cloudasr.config` is the release
profile; `TUYA_T5AI_CORE_dual_cloudasr_debug.config` is for development only,
and a debug image is never shared. Selecting a profile can rewrite
`app_default.config`: check `git status` and do not include that file by
accident. Details and flashing: [firmware/app/README.md](firmware/app/README.md).

## Companion on macOS and other platforms

The Companion is built and tested on Windows today, and help with other
platforms is welcome. Platform-specific code is gated with `cfg(windows)` or
`cfg(target_os = ...)`:

| File (`companion/src-tauri/`) | Platform-specific part |
|---|---|
| `Cargo.toml` | `tauri`, `reqwest`, `keyring`, `cpal` and `native-tls` are Windows-only dependencies |
| `src/lib.rs` | the Tauri application (`mod app`, `run()`) is Windows-only; other hosts get a stub |
| `src/inject.rs` | key injection (`user32` on Windows); other hosts get a stub |
| `src/asr.rs`, `src/stream_asr/` (`capture.rs`, `model.rs`, `sherpa.rs`, `ws.rs`) | system speech input, microphone capture, model download, speech engine loading |
| `src/secrets.rs` | API key storage (Windows Credential Manager) |
| `src/kirocrew_control.rs` | Kiro Crew window control: Win32 backend, stub elsewhere |
| `src/crew_identity.rs` | which process owns the local Kiro Crew port: Windows and Linux implemented, macOS returns "not implemented" (fails closed) |
| `src/ble_pairing.rs` | pairing key storage: Windows DPAPI; other hosts keep it in a `0600` file |
| `src/crew_api.rs`, `src/status_sink.rs` | `cfg(unix)` file-permission checks on local secret and handoff files |
| `tauri.conf.json` | the bundle target is `nsis` (Windows installer) |

Find the current list with
`git grep -n "cfg(.*\(windows\|target_os\|unix\)" -- companion/src-tauri`.
Keep the pure logic (protocol, state, stores) free of platform code so it
stays testable on any host. Put a new platform backend behind the same trait or
function the existing code uses, and keep the stub for the other platforms.
Say in your pull request which OS you tested on.

## Hardware and documents

`hardware/` holds exports of the board design. Open an issue to propose a
design change instead of editing the exported files. Documents under `docs/`
are Chinese-first; README files are bilingual. Code comments are in English.

## Commits

Write commit messages in English, in Conventional Commits style:
`<type>(<scope>): <summary>`.

- Types in use: `feat`, `fix`, `docs`, `test`, `refactor`, `build`, `ci`, `chore`, `security`.
- Scopes: `firmware`, `companion`, `hardware`, or a combination such as `docs,firmware`.
- Examples: `fix(firmware): BLE pairing window cannot be used up`,
  `feat(companion): read-only Device info block`,
  `docs: Device info page and the device_info frame`.
- One logical change per commit, with its tests in the same commit.

## What never goes into git

- Credentials of any kind: API keys, tokens, passwords, certificates.
- A Tuya product ID (PID), device UUID or AuthKey. The repository ships with an
  empty PID; set your own as described in [firmware/app/README.md](firmware/app/README.md).
- Firmware binaries (`*.bin`, `*.elf`, `*.hex`). An image built with a device
  licence carries that device's UUID and AuthKey, and a file committed once stays
  in the history. `.gitignore` excludes them; do not force-add one.
- Writes to the flash region at `0x7ce000` (custom boot screen) are not part of any
  change: it is not verified as free on every build.

## Licensing

Firmware, Companion, tools and documentation are licensed under Apache-2.0
(see the `LICENSE` file). Hardware design files keep the licence of their
OSHWHub project page.

- **Your contribution is licensed under the project's licence.** By submitting a
  pull request you confirm that you wrote the change or have the right to
  submit it, and that it is licensed under the licence of the files it
  changes (Apache-2.0 for software and documents, the OSHWHub licence for
  hardware files). We may ask you to confirm the licensing of your contribution.
- **Third-party code, fonts and images** must be compatible with distribution
  under Apache-2.0. Permissive licences (MIT, BSD, ISC, Apache-2.0, and OFL for
  fonts) are fine. Copyleft licences (GPL, LGPL, AGPL), licences that forbid
  commercial use or redistribution, and material with no licence are not
  accepted unless a maintainer agrees in the issue first. Name the project and
  its licence in the pull request, keep its licence text and attribution with it
  (fonts: the licence texts in `firmware/app/third_party/fonts/`),
  and the maintainers will update `NOTICE`.

## Security

If you discover a potential security issue in this project, report it
privately with **Report a vulnerability** on the repository's Security tab.
Do **not** create a public GitHub issue, pull request or discussion for it.
Details are in [SECURITY.md](SECURITY.md).
