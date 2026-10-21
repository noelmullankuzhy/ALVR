# Build the Windows streamer from the Mac

From this fork, run:

```sh
sh tools/lumen/pc-build-streamer.sh develop
# Branches, tags and full/abbreviated commit hashes are accepted.
```

Requires Mac Git, Python 3, SSH/SCP, the `win-workstation` SSH alias, and
`/Users/noel/Projects/Personal/vr-gaming/tools/pcps.sh`. PowerShell is sent through
that helper as an encoded command because the PC's SSH shell is cmd.exe.
The PC needs existing Git, VS 2022 C++ Build Tools and a Windows SDK, and network
access to Rust, GitHub and crates.io. No interactive installer is used.

The command bundles the selected **committed** tree and its history, including
submodule gitlinks and `.gitmodules`. The PC fetches the bundle, checks out the
exact commit, and downloads the pinned OpenVR submodule from GitHub. Uncommitted
Mac changes are excluded. A dirty PC source checkout is refused.

Everything created on the PC lives under
`C:\Users\noelm\claude-tmp\alvr-build`:

| Path | Contents |
| --- | --- |
| `incoming` | Uploaded Git bundle and build script |
| `src` | Reused source checkout, submodules, Cargo target/build directories |
| `tools\cargo`, `tools\rustup` | User-local rustup, pinned Rust 1.82.0 MSVC toolchain and shared Cargo downloads |
| `tools\clang+llvm-18.1.8-x86_64-pc-windows-msvc` | Portable official LLVM archive, only when existing libclang is absent |
| `tools\locked-cargo` | Process-local wrapper enforcing the committed Cargo.lock in child builds |
| `out\<12-character-commit>\alvr_streamer_windows` | Copied release package, replaced when that commit is rebuilt |
| `TOOLS.txt` | Installed/selected tool versions, paths and libclang hash |

Rustup uses `--no-modify-path`; `PATH`, `LIBCLANG_PATH`, Cargo/Rustup homes and VS
environment changes affect only the build process. Existing MSVC is discovered
with vswhere. Nothing uses Chocolatey, winget, setx, registry edits, driver
registration, or elevation. No dashboard, SteamVR, Steam or game is launched or
stopped. The live streamer in Downloads is never accessed.

The v20 build command is `cargo xtask build-streamer --release`, without `--gpl`.
The GM explicitly excluded `prepare-deps`: at e3fd448 its Windows implementation
elevates Chocolatey, downloads GPL-only encoder dependencies and calls setx.
The reviewed call path is `main.rs`'s `build-streamer` arm →
`build::build_streamer` → separate Cargo builds for server and dashboard; it does
not invoke dependency preparation. Bindgen in `server_openvr/build.rs` requires
libclang even without GPL; cc uses existing MSVC. See the checked-out
[v20 build documentation](../../wiki/Building-From-Source.md).

Cargo.lock is enforced for xtask and child Cargo builds. Toolchain versions are
pinned; this is a repeatable source/dependency build, not a claim of bit-for-bit
PE reproducibility across different MSVC/SDK versions or timestamps. Later
commits must remain compatible with the pinned toolchain and v20 xtask/layout.
If dependencies, licensing or the build path require a decision, stop and review
instead of enabling GPL or running dependency preparation.

Stdout contains one JSON line with success, resolved commit, output directory,
elapsed seconds, SHA256 and Windows product/file metadata for the driver DLL and
dashboard EXE. Metadata is read with `FileVersionInfo`, without executing either
binary. A binary without a version resource has empty version fields.
Logs and the latest JSON are kept in Mac `local-scratch/pc-build-streamer/`.
Concurrent runs from this checkout are refused. Do not run separate Mac checkouts
against the shared PC build directory concurrently.

The script refuses to start below 60 GiB free on C: and checks a 30 GiB build
directory/growth budget at major stages and every 15 seconds during Cargo builds.
An over-budget build terminates only its own Cargo process tree. Old outputs count toward the budget;
remove unneeded build outputs before accumulating many commits.

Verification on 21 October 2026: unmodified develop
`e3fd448029c795b1b2d5835c84c6588bf01bae0d` failed with E0004 in the two
server_core matches missing PSVR2Sense. The GM authorized backporting upstream
`669b5411` on `feature/psvr2-server-backport`, producing
`d45b991cefcc5a22e3fc42c515e5f6bbb6ac3230`. Only the inputs.rs Lazy conflict
needed resolution; settings.rs already contained the variant. No extra source
or resource files were added: the upstream tree contains no PSVR2 model/profile
assets to import, so runtime PSVR2 resource availability remains unverified.

Both release binaries compiled on the first backport attempt (109 seconds
end to end), but PowerShell returned a null ExitCode from Start-Process and
the wrapper reported failure. The pipeline now starts Diagnostics.Process
directly, retaining its handle, setting the source working directory and draining
both output pipes concurrently before reporting the real exit code. Budget
monitoring remains active during compilation; captured Cargo output is printed
after the process exits.

Two consecutive runs of the corrected pipeline both exited **0** in **15 seconds**
each, with `ok: true` and the exact backport head. Both reused the existing
Rust 1.82.0/LLVM tools and Cargo artifacts, with no tool installation or crate
downloads. Dashboard product/file version is **20.14.1**; the driver has no
Windows version resource. Both runs produced identical SHA256 values:

| File | SHA256 |
| --- | --- |
| `bin\win64\driver_alvr_server.dll` | `464336a9dbdc87fc28177cd7959c6a5ae6ae1a2e688f6411549435f173dc8f76` |
| `ALVR Dashboard.exe` | `b00cbacf9a96e40e6823908a37330b1c79de62cbae9bdae648f32e59f517d9af` |

Evidence: Mac `local-scratch/pc-build-streamer/psvr2-run1.json`,
`psvr2-run2.json`, and matching `.log` files. Earlier failure logs remain there.
No built executable was launched; runtime streaming was not tested.

To remove this pipeline's PC files, delete only
`C:\Users\noelm\claude-tmp\alvr-build` when no build is active. The self-contained
toolchains require no uninstaller or environment/registry cleanup. Remove Mac
`local-scratch/pc-build-streamer/` to discard its bundle and logs. A stale Mac
`lock` directory may be removed after verifying no build is active.

Runtime FoV validation: see [the opt-in angle chart and PNG dump](angle-chart.md).
