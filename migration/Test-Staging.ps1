# Runs on a disposable VM; TLS certificate is only for localhost, NOT an updater signing key.
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$dir = New-Item "$env:RUNNER_TEMP/watchman-staging" -ItemType Directory
$latest = New-Item "$dir/releases/latest/download" -ItemType Directory -Force
$versionDir = New-Item "$dir/releases/download/v2.0.0" -ItemType Directory -Force
$name = 'Watchman_2.0.0_x64-setup.exe'
$original = "builds/staged/$name"
$asset = "$versionDir/$name"
$file = "$latest/latest.json"
Copy-Item $original $asset
$feed = Get-Content builds/staged/latest.json -Raw | ConvertFrom-Json
$entry = $feed.platforms.'windows-x86_64'
if($entry.url -cne "https://github.com/rayhan138/Watchman/releases/download/v2.0.0/$name"){throw 'Wrong production URL'}
$entry.url = "https://localhost:45443/releases/download/v2.0.0/$name"
function Save-Feed { $feed | ConvertTo-Json -Depth 8 | Set-Content $file -Encoding utf8NoBOM }
Save-Feed
$cert = "$env:RUNNER_TEMP/localhost.pem"
$key = "$env:RUNNER_TEMP/localhost.key"
openssl req -x509 -newkey rsa:2048 -nodes -keyout $key -out $cert -days 1 -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost' *> $null
# CurrentUser Root import can open an unattended trust-confirmation dialog.
# This VM is disposable; use the local-machine store and remove this certificate in finally.
certutil -f -addstore Root $cert
$server = Start-Process python -ArgumentList @('migration/staging_server.py', $dir, $cert, $key) -PassThru
try {
    $probe = 'builds/native/Release/WatchmanUpdateProtocolProbe.exe'
    Copy-Item migration/verify/target/release/watchman-verify-release.exe (Split-Path $probe) -Force
    function Probe($expected){
        $PSNativeCommandUseErrorActionPreference = $false
        $result = & $probe 2>&1
        if($LASTEXITCODE -ne $expected){throw "Probe returned $LASTEXITCODE instead of ${expected}: $result"}
    }
    Start-Sleep 2
    Probe 0
    New-Item "$dir/redirect-test" -ItemType File | Out-Null
    Probe 2
    Remove-Item "$dir/redirect-test"
    $feed.platforms.PSObject.Properties.Remove('windows-x86_64'); Save-Feed; Probe 2
    $feed.platforms | Add-Member -NotePropertyName 'windows-x86_64' -NotePropertyValue $entry
    $feed.version = '1.0.5'; Save-Feed; Probe 3
    $feed.version = '2.0.0'
    $url = $entry.url; $entry.url = 'http://localhost/unsigned'; Save-Feed; Probe 2
    $entry.url = $url; $signature = $entry.signature; $entry.signature = 'AA=='; Save-Feed; Probe 2
    $entry.signature = $signature; Save-Feed
    $stream = [IO.File]::OpenWrite($asset); try { $stream.WriteByte(0) } finally { $stream.Dispose() }
    Probe 2
    Copy-Item $original $asset -Force
    # Real installed v1; the staging probe launches the locked, verified candidate.
    & "$PSScriptRoot/Test-Upgrade.ps1" -ProtocolProbe $probe
} finally {
    Stop-Process -Id $server.Id -ErrorAction SilentlyContinue
    $thumb = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cert).Thumbprint
    Remove-Item "Cert:\LocalMachine\Root\$thumb" -ErrorAction SilentlyContinue
    Remove-Item $key -ErrorAction SilentlyContinue
}
