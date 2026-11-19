# Requires x64 MSVC, CMake, Rust and the locked npm packaging CLI. No tauri build.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-Location "$PSScriptRoot/.."
cmake -S native -B builds/native -A x64 -DBUILD_TESTING=ON
cmake --build builds/native --config Release --parallel 4
ctest --test-dir builds/native -C Release --output-on-failure
cargo build --locked --release --manifest-path migration/verify/Cargo.toml
$null = New-Item src-tauri/target/release -ItemType Directory -Force
Copy-Item builds/native/Release/Watchman.exe src-tauri/target/release/watchman.exe
Copy-Item migration/verify/target/release/watchman-verify-release.exe src-tauri/
Copy-Item native/fonts/*-*.txt src-tauri/
# Bundle only: same NSIS template, binary name, publisher and registry identity as v1.
npm ci --ignore-scripts --no-audit --no-fund
& ./node_modules/.bin/tauri.cmd bundle --config src-tauri/native-package.json --bundles nsis --no-sign --ci
$null = New-Item builds/staged -ItemType Directory -Force
Copy-Item src-tauri/target/release/bundle/nsis/Watchman_2.0.0_x64-setup.exe builds/staged/
