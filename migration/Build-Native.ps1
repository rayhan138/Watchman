# Requires x64 MSVC, CMake, Rust and the locked npm packaging CLI. No tauri build.
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-Location "$PSScriptRoot/.."
cmake -S native -B builds/native -A x64 -DBUILD_TESTING=ON
$PSNativeCommandUseErrorActionPreference = $false
cmake --build builds/native --config Release --parallel 4 2>&1 | Tee-Object builds/compile.log
if ($LASTEXITCODE) {
    Select-String 'error (C|LNK|MSB)[0-9]+' builds/compile.log | Select-Object -First 8 | ForEach-Object { Write-Host "::error::$($_.Line)" }
    throw 'MSVC build failed'
}
$PSNativeCommandUseErrorActionPreference = $true
ctest --test-dir builds/native -C Release --output-on-failure --timeout 45
# The installed verifier must not require a separate Visual C++ runtime.
$previousRustflags = $env:RUSTFLAGS
try {
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    cargo build --locked --release --manifest-path migration/verify/Cargo.toml
} finally { $env:RUSTFLAGS = $previousRustflags }
$null = New-Item src-tauri/target/release -ItemType Directory -Force
Copy-Item builds/native/Release/Watchman.exe src-tauri/target/release/watchman.exe
Copy-Item migration/verify/target/release/watchman-verify-release.exe src-tauri/
Copy-Item native/fonts/*-*.txt src-tauri/
# Bundle only: same NSIS template, binary name, publisher and registry identity as v1.
npm ci --ignore-scripts --no-audit --no-fund
$PSNativeCommandUseErrorActionPreference = $false
& ./node_modules/.bin/tauri.cmd bundle --config src-tauri/native-package.json --bundles nsis --no-sign --ci 2>&1 | Tee-Object builds/bundle.log
if ($LASTEXITCODE) { Get-Content builds/bundle.log -Tail 8 | ForEach-Object {Write-Host "::error::$_"}; throw 'Packaging failed' }
$PSNativeCommandUseErrorActionPreference = $true
$null = New-Item builds/staged -ItemType Directory -Force
Copy-Item src-tauri/target/release/bundle/nsis/Watchman_2.0.0_x64-setup.exe builds/staged/
(Get-FileHash builds/native/Release/Watchman.exe).Hash | Set-Content builds/staged/Watchman.exe.sha256
