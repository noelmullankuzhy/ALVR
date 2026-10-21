param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{40}$')][string]$Commit
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$timer = [Diagnostics.Stopwatch]::StartNew()
$base = 'C:\Users\noelm\claude-tmp\alvr-build'
$src = Join-Path $base 'src'
$tools = Join-Path $base 'tools'
$rustVersion = '1.82.0'
$hostTriple = 'x86_64-pc-windows-msvc'
$initialFree = (Get-PSDrive C).Free

function Check-Budget {
    $free = (Get-PSDrive C).Free
    if ($free -lt 60GB) { throw 'C: has less than 60 GB free' }
    if (($initialFree - $free) -gt 30GB) { throw 'Build exceeded its 30 GB disk growth budget' }
    $used = (Get-ChildItem $base -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($used -gt 30GB) { throw 'alvr-build exceeds its 30 GB disk budget; remove old outputs before retrying' }
}
function Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}
function Download([string]$url, [string]$destination) {
    if (!(Test-Path $destination)) {
        Native 'curl.exe' @('-fL', '--retry', '2', '--output', "$destination.partial", $url)
        Move-Item "$destination.partial" $destination -Force
    }
}
try {
    if ($initialFree -lt 60GB) { throw 'C: has less than 60 GB free' }
    New-Item -ItemType Directory -Force $base,$tools | Out-Null
    Check-Budget
    # All toolchain state and environment changes are scoped to this directory/process.
    $env:CARGO_HOME = Join-Path $tools 'cargo'
    $env:RUSTUP_HOME = Join-Path $tools 'rustup'
    $env:PATH = "$env:CARGO_HOME\bin;$env:PATH"
    $rustup = Join-Path $env:CARGO_HOME 'bin\rustup.exe'
    if (!(Test-Path $rustup)) {
        $installer = Join-Path $tools 'rustup-init.exe'
        $url = 'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe'
        Download $url $installer
        Download "$url.sha256" "$installer.sha256"
        $expected = ((Get-Content "$installer.sha256" -Raw) -split '\s+')[0]
        if ((Get-FileHash $installer -Algorithm SHA256).Hash -ne $expected) { throw 'rustup checksum mismatch' }
        $env:RUSTUP_INIT_SKIP_PATH_CHECK = 'yes'
        Native $installer @('-y', '--no-modify-path', '--profile', 'minimal', '--default-host', $hostTriple, '--default-toolchain', $rustVersion)
    }
    Native $rustup @('set', 'default-host', $hostTriple)
    $installed = & $rustup toolchain list
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list Rust toolchains' }
    if (!($installed -match "^$rustVersion-$hostTriple")) {
        Native $rustup @('toolchain', 'install', $rustVersion, '--profile', 'minimal')
    }
    $env:RUSTUP_TOOLCHAIN = "$rustVersion-$hostTriple"
    # Import the existing VS environment without cmd.exe quote nesting or persistent changes.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Existing MSVC x64 build tools not found; installation decision required' }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    # build.rs unconditionally uses bindgen. Prefer existing libclang; otherwise portable LLVM.
    $llvmDir = Join-Path $tools 'clang+llvm-18.1.8-x86_64-pc-windows-msvc'
    $candidates = @(
        "$llvmDir\bin\libclang.dll",
        "$vs\VC\Tools\Llvm\x64\bin\libclang.dll",
        'C:\Program Files\LLVM\bin\libclang.dll'
    )
    $libclang = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$libclang) {
        $archive = Join-Path $tools 'llvm-18.1.8.tar.xz'
        Download 'https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang%2Bllvm-18.1.8-x86_64-pc-windows-msvc.tar.xz' $archive
        # Pinned from the official release archive used for the verified build.
        if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne '22C5907DB053026CC2A8FF96D21C0F642A90D24D66C23C6D28EE7B1D572B82E8') {
            throw 'LLVM archive checksum mismatch'
        }
        Native 'tar.exe' @('-xf', $archive, '-C', $tools)
        $libclang = "$llvmDir\bin\libclang.dll"
        if (!(Test-Path $libclang)) { throw 'LLVM archive did not contain libclang.dll' }
    }
    $env:LIBCLANG_PATH = Split-Path $libclang
    @(
        "Recorded: $([DateTime]::UtcNow.ToString('o'))"
        "Rustup/Cargo homes: $env:RUSTUP_HOME ; $env:CARGO_HOME"
        (& $rustup --version)
        (& rustc -Vv)
        (& cargo -V)
        (& git --version)
        "VS: $vs"
        "MSVC: $env:VCToolsVersion; SDK: $env:WindowsSDKVersion"
        "libclang: $libclang; FileVersion: $((Get-Item $libclang).VersionInfo.FileVersion)"
        "libclang SHA256: $((Get-FileHash $libclang -Algorithm SHA256).Hash)"
        "LLVM archive SHA256 (when downloaded): 22c5907db053026cc2a8ff96d21c0f642a90d24d66c23c6d28ee7b1d572b82e8"
    ) | Set-Content (Join-Path $base 'TOOLS.txt')
    $bundle = Join-Path $base 'incoming\source.bundle'
    if (!(Test-Path "$src\.git")) { Native 'git' @('clone', '--no-checkout', $bundle, $src) }
    Set-Location $src
    if ((& git status --porcelain)) { throw 'Build source has local changes; refusing checkout' }
    Native 'git' @('fetch', $bundle, '+refs/lumen/pc-build-input:refs/remotes/bundle/input')
    Native 'git' @('checkout', '--detach', $Commit)
    if ((& git rev-parse HEAD) -ne $Commit) { throw 'Commit mismatch' }
    # Refuse source whose build-streamer implementation has started invoking prepare-deps.
    $buildSource = Get-Content 'alvr\xtask\src\build.rs' -Raw
    if ($buildSource -match 'prepare[_-](deps|server_deps|windows_deps)') { throw 'build.rs references dependency preparation; review call path before building' }
    Native 'git' @('submodule', 'sync', '--recursive')
    Native 'git' @('submodule', 'update', '--init', '--recursive')
    if (!(Test-Path 'Cargo.lock')) { throw 'Committed Cargo.lock required for repeatable builds' }
    # Enforce the committed lockfile for xtask AND its child Cargo processes.
    # Cargo offline alone does not enforce the lock, so use a process-local cargo shim.
    $realCargo = (& $rustup which cargo).Trim()
    $shim = Join-Path $tools 'locked-cargo'
    New-Item -ItemType Directory -Force $shim | Out-Null
    $env:LUMEN_REAL_CARGO = $realCargo
    $proxySource = @'
use std::{env, process::{Command, exit}};
fn main() {
    let status = Command::new(env::var_os("LUMEN_REAL_CARGO").expect("real cargo path"))
        .args(env::args_os().skip(1)).arg("--locked").status().expect("run cargo");
    exit(status.code().unwrap_or(1));
}
'@
    $proxyFile = Join-Path $shim 'proxy.rs'
    $proxyExe = Join-Path $shim 'cargo.exe'
    if (!(Test-Path $proxyExe) -or !(Test-Path $proxyFile) -or (Get-Content $proxyFile -Raw).Trim() -ne $proxySource.Trim()) {
        $proxySource | Set-Content $proxyFile -Encoding Ascii
        Native 'rustc' @('--edition=2021', $proxyFile, '-o', $proxyExe)
    }
    $env:PATH = "$shim;$env:PATH"
    $env:CARGO_ALIAS_XTASK = 'run --locked -p alvr_xtask --'
    Check-Budget
    $build = Start-Process -FilePath $realCargo -ArgumentList @('xtask', 'build-streamer', '--release') -NoNewWindow -PassThru
    try {
        while (!$build.WaitForExit(15000)) { Check-Budget }
        # Refresh the process handle before reading ExitCode on Windows PowerShell 5.1.
        $build.WaitForExit()
        if ($build.ExitCode -ne 0) { throw "cargo xtask build-streamer failed with exit code $($build.ExitCode)" }
    } catch {
        if (!$build.HasExited) {
            # Terminate only this build's process tree when its budget/check fails.
            & taskkill.exe /PID $build.Id /T /F | Out-Null
        }
        throw
    }
    Native 'git' @('diff', '--exit-code', '--', 'Cargo.lock')
    Check-Budget
    $out = Join-Path $base "out\$($Commit.Substring(0,12))\alvr_streamer_windows"
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    Copy-Item 'build\alvr_streamer_windows' $out -Recurse
    $files = [ordered]@{}
    foreach ($name in @('bin\win64\driver_alvr_server.dll', 'ALVR Dashboard.exe')) {
        $path = Join-Path $out $name
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        $files[$name] = [ordered]@{
            sha256 = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
            product_version = $version.ProductVersion
            file_version = $version.FileVersion
        }
    }
    Check-Budget
    [ordered]@{ok=$true; commit=$Commit; out_dir=$out; seconds=[math]::Round($timer.Elapsed.TotalSeconds,2); files=$files} | ConvertTo-Json -Depth 5 -Compress
    exit 0
} catch {
    [ordered]@{ok=$false; commit=$Commit; seconds=[math]::Round($timer.Elapsed.TotalSeconds,2); error=$_.Exception.Message} | ConvertTo-Json -Compress
    exit 1
}
