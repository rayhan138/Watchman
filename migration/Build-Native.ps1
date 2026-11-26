# Requires x64 MSVC, CMake, Rust and the locked npm packaging CLI. No tauri build.
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-Location "$PSScriptRoot/.."
$versionHeader = Get-Content native/version.h -Raw
$nativeVersion = [regex]::Match($versionHeader, '(?m)^#define WATCHMAN_VERSION_TEXT "([0-9]+\.[0-9]+\.[0-9]+)"\r?$').Groups[1].Value
$numericVersion = @('MAJOR','MINOR','PATCH' | ForEach-Object {
    [regex]::Match($versionHeader, "(?m)^#define WATCHMAN_VERSION_$_ ([0-9]+)\r?$").Groups[1].Value
}) -join '.'
if (!$nativeVersion -or $numericVersion -cne $nativeVersion) { throw 'Text and numeric native versions do not match' }
$package = Get-Content src-tauri/native-package.json -Raw | ConvertFrom-Json
$config = Get-Content src-tauri/tauri.conf.json -Raw | ConvertFrom-Json
if ($package.version -cne $nativeVersion) { throw "Native and NSIS package versions must match ($nativeVersion)" }
if ($config.version -cne '1.0.5') { throw 'Keep the historical Tauri app configuration at 1.0.5; the native package override owns the executable version' }
if ($package.bundle.publisher -cne 'Noman Dhoni') { throw 'NSIS publisher must be exactly Noman Dhoni' }
cmake -S native -B builds/native -A x64 -DBUILD_TESTING=ON -DWATCHMAN_BUILD_RUNTIME_SMOKE=ON
$PSNativeCommandUseErrorActionPreference = $false
cmake --build builds/native --config Release --parallel 4 2>&1 | Tee-Object builds/compile.log
if ($LASTEXITCODE) {
    Select-String 'error (C|LNK|MSB)[0-9]+' builds/compile.log | Select-Object -First 8 | ForEach-Object { Write-Host "::error::$($_.Line)" }
    throw 'MSVC build failed'
}
$PSNativeCommandUseErrorActionPreference = $true
ctest --test-dir builds/native -C Release --output-on-failure --timeout 45
if ($env:GITHUB_ACTIONS -eq 'true') {
    # The task registration must also work from a real standard (non-admin) token.
    & ./native/tests/startup_registration_standard_user.ps1 `
        -Executable builds/native/Release/StartupRegistrationTest.exe `
        -ReportPath builds/native/startup-standard-user-tests.txt
    # Exercise the actual desktop, startup migration, profile files, and production engine
    # on this disposable GitHub-hosted Windows runner before signing the installer.
    & ./native/tests/windows_smoke_test.ps1 `
        -Executable builds/native/Release/Watchman.exe `
        -RuntimeProbe builds/native/Release/WatchmanRuntimeSmoke.exe `
        -OutputDirectory builds/smoke-results
} else {
    Write-Warning 'Skipped standard-user account and interactive desktop smoke checks; these destructive/interactive checks run only on disposable GitHub Actions Windows runners.'
}
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
# Bundle only: same NSIS template, binary name and uninstall identity; publisher is intentionally updated.
npm ci --ignore-scripts --no-audit --no-fund
$PSNativeCommandUseErrorActionPreference = $false
& ./node_modules/.bin/tauri.cmd bundle --config src-tauri/native-package.json --bundles nsis --no-sign --ci 2>&1 | Tee-Object builds/bundle.log
if ($LASTEXITCODE) { Get-Content builds/bundle.log -Tail 8 | ForEach-Object {Write-Host "::error::$_"}; throw 'Packaging failed' }
$PSNativeCommandUseErrorActionPreference = $true
$null = New-Item builds/staged -ItemType Directory -Force
Copy-Item "src-tauri/target/release/bundle/nsis/Watchman_${nativeVersion}_x64-setup.exe" builds/staged/
(Get-FileHash builds/native/Release/Watchman.exe).Hash | Set-Content builds/staged/Watchman.exe.sha256
