# Disposable Windows runner only. Does not read latest/download or publish anything.
$ErrorActionPreference = 'Stop'
$dir = New-Item -ItemType Directory (Join-Path $env:RUNNER_TEMP 'watchman-v1-baseline')
$name = 'Watchman_1.0.5_x64-setup.exe'
$setup = Join-Path $dir $name
$repo = 'rayhan138/Watchman'
gh release download v1.0.5 --repo $repo --dir $dir -p $name -p "$name.sig" -p latest.json
if ($LASTEXITCODE) { throw 'Could not acquire the released baseline' }
$feed = Get-Content -Raw (Join-Path $dir latest.json) | ConvertFrom-Json
$entry = $feed.platforms.'windows-x86_64'
$sig = (Get-Content -Raw "$setup.sig").Trim()
if ($feed.version -cne '1.0.5' -or $entry.signature -cne $sig -or
    $entry.url -cne "https://github.com/$repo/releases/download/v1.0.5/$name") {
    throw 'Released manifest does not match the detached signature and expected asset'
}
[IO.File]::WriteAllBytes("$setup.minisig", [Convert]::FromBase64String($sig))
$crate = Join-Path $PSScriptRoot 'verify'
cargo build --release --locked --manifest-path "$crate/Cargo.toml"
if ($LASTEXITCODE) { throw 'Failed to build offline verifier' }
$verify = Join-Path $crate 'target/release/watchman-verify-release.exe'
& $verify $setup "$setup.minisig"
if ($LASTEXITCODE) { throw 'Untrusted installer' }
$bad = [IO.File]::ReadAllBytes($setup)
$bad[-1] = $bad[-1] -bxor 1
[IO.File]::WriteAllBytes("$setup.bad", $bad)
& $verify "$setup.bad" "$setup.minisig"
if ($LASTEXITCODE -eq 0) { throw 'Corrupted installer passed verification' }
Get-FileHash $setup | Format-List
$p = Start-Process $setup -ArgumentList '/S' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "v1.0.5 install failed: $($p.ExitCode)" }
$entries = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' |
    Where-Object { $_.DisplayName -eq 'Watchman' })
if ($entries.Count -ne 1 -or $entries[0].DisplayVersion -ne '1.0.5') { throw 'Unexpected machine uninstall registration' }
$entries | Format-List PSPath, DisplayVersion, InstallLocation, UninstallString
exit 0
