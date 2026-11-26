# Disposable Windows runner only. Install the immutable public v2.0.1 baseline.
param([switch]$NonDefaultPath)
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$version = '2.0.1'
$dir = New-Item -ItemType Directory (Join-Path $env:RUNNER_TEMP 'watchman-v201-baseline')
$name = "Watchman_${version}_x64-setup.exe"
$setup = Join-Path $dir $name
$repo = 'rayhan138/Watchman'
gh release download "v$version" --repo $repo --dir $dir -p $name -p "${name}.sig" -p latest.json
if ($LASTEXITCODE) { throw 'Could not acquire the immutable v2.0.1 baseline' }

# Pin the current public stable assets as well as verifying the updater signature.
$signatureName = "${name}.sig"
$expected = @{}
$expected[$name] = 'fcaa5d43f179443e904a0193e2a0f20faefd0d9a5c04bf315666ea81f39380cc'
$expected[$signatureName] = '6e5768a4357cecbffeb062377f3cec8d6e90978615eadab5b584c4978ce20ff1'
$expected['latest.json'] = 'f0fa7cf26774c5c4b62e8fdb44a6211bd6a89fbbee10160cc9f9b828a6a6bac4'
foreach ($asset in $expected.Keys) {
    $actual = (Get-FileHash (Join-Path $dir $asset) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $expected[$asset]) { throw "Public v2.0.1 asset hash mismatch: $asset ($actual)" }
}
$feed = Get-Content -Raw (Join-Path $dir latest.json) | ConvertFrom-Json
$entry = $feed.platforms.'windows-x86_64'
$sig = (Get-Content -Raw "${setup}.sig").Trim()
if ($feed.version -cne $version -or $entry.signature -cne $sig -or
    $entry.url -cne "https://github.com/$repo/releases/download/v$version/$name") {
    throw 'Released v2.0.1 manifest does not match its detached signature and expected asset'
}
[IO.File]::WriteAllBytes("${setup}.minisig", [Convert]::FromBase64String($sig))
$crate = Join-Path $PSScriptRoot 'verify'
cargo build --locked --release --manifest-path "$crate/Cargo.toml"
if ($LASTEXITCODE) { throw 'Failed to build offline verifier' }
$verify = Join-Path $crate 'target/release/watchman-verify-release.exe'
& $verify $setup "${setup}.minisig"
if ($LASTEXITCODE) { throw 'Released v2.0.1 installer is not trusted by the pinned v1.x key' }
$pe = [Diagnostics.FileVersionInfo]::GetVersionInfo($setup)
if ($pe.ProductVersion -cne $version) { throw 'Released v2.0.1 installer PE version mismatch' }
$installPath = if ($NonDefaultPath) { "$env:ProgramFiles\Watchman Acceptance" } else { "$env:ProgramFiles\Watchman" }
$argsLine = if ($NonDefaultPath) { "/S /D=$installPath" } else { '/S' }
$p = Start-Process $setup -ArgumentList $argsLine -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "v2.0.1 install failed: $($p.ExitCode)" }
$entries = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' |
    Where-Object { $_.DisplayName -eq 'Watchman' })
if ($entries.Count -ne 1 -or $entries[0].DisplayVersion -ne $version -or $entries[0].Publisher -cne 'Noman Dhoni') {
    throw 'Unexpected v2.0.1 machine uninstall registration or publisher'
}
if ($entries[0].InstallLocation.Trim('"') -ne $installPath) { throw 'Custom v2.0.1 path was not used' }
$app = Join-Path (Split-Path $entries[0].UninstallString.Trim('"')) 'watchman.exe'
if (!(Test-Path $app)) { throw 'Released v2.0.1 executable is missing from its install path' }
$key = Get-Item 'HKLM:\SOFTWARE\Noman Dhoni\Watchman'
try { if ($key.GetValue('') -ne $installPath) { throw 'v2.0.1 publisher install-location key mismatch' } }
finally { $key.Close() }
$entries | Format-List PSPath, DisplayVersion, Publisher, InstallLocation, UninstallString
exit 0
