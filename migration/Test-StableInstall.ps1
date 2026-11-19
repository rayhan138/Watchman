# Disposable Windows runner only. Install the immutable public v2.0.0 baseline.
param([switch]$NonDefaultPath)
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$dir = New-Item -ItemType Directory (Join-Path $env:RUNNER_TEMP 'watchman-v2-baseline')
$name = 'Watchman_2.0.0_x64-setup.exe'
$setup = Join-Path $dir $name
$repo = 'rayhan138/Watchman'
gh release download v2.0.0 --repo $repo --dir $dir -p $name -p "$name.sig" -p latest.json
if ($LASTEXITCODE) { throw 'Could not acquire the released v2.0.0 baseline' }
$feed = Get-Content -Raw (Join-Path $dir latest.json) | ConvertFrom-Json
$entry = $feed.platforms.'windows-x86_64'
$sig = (Get-Content -Raw "$setup.sig").Trim()
if ($feed.version -cne '2.0.0' -or $entry.signature -cne $sig -or
    $entry.url -cne "https://github.com/$repo/releases/download/v2.0.0/$name") {
    throw 'Released v2.0.0 manifest does not match its detached signature and expected asset'
}
[IO.File]::WriteAllBytes("$setup.minisig", [Convert]::FromBase64String($sig))
$crate = Join-Path $PSScriptRoot 'verify'
cargo build --release --locked --manifest-path "$crate/Cargo.toml"
if ($LASTEXITCODE) { throw 'Failed to build offline verifier' }
$verify = Join-Path $crate 'target/release/watchman-verify-release.exe'
& $verify $setup "$setup.minisig"
if ($LASTEXITCODE) { throw 'Released v2.0.0 installer is not trusted by the pinned v1.x key' }
$pe = [Diagnostics.FileVersionInfo]::GetVersionInfo($setup)
if ($pe.ProductVersion -cne '2.0.0') { throw 'Released v2.0.0 installer PE version mismatch' }
Get-FileHash $setup | Format-List
# Match the existing v1 baseline fixture: a custom path with spaces exposes path regressions.
$installPath = "$env:ProgramFiles\Watchman Acceptance"
$argsLine = if ($NonDefaultPath) { "/S /D=$installPath" } else { '/S' }
$p = Start-Process $setup -ArgumentList $argsLine -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "v2.0.0 install failed: $($p.ExitCode)" }
$entries = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' |
    Where-Object { $_.DisplayName -eq 'Watchman' })
if ($entries.Count -ne 1 -or $entries[0].DisplayVersion -ne '2.0.0') { throw 'Unexpected v2.0.0 machine uninstall registration' }
if ($NonDefaultPath -and $entries[0].InstallLocation.Trim('"') -ne $installPath) { throw 'Custom v2.0.0 path was not used' }
if ($entries[0].Publisher -cne 'blinkeye') { throw 'Expected the unchanged pre-hotfix publisher registry key for the v2.0.0 baseline' }
$entries | Format-List PSPath, DisplayVersion, Publisher, InstallLocation, UninstallString
exit 0
