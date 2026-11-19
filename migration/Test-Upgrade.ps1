# Only on a freshly provisioned Windows runner, immediately after Test-LegacyInstall.
param([string]$Setup, [string]$ProtocolProbe)
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$version = (Get-Content src-tauri/native-package.json -Raw | ConvertFrom-Json).version
if (!$Setup) { $Setup = "builds/staged/Watchman_${version}_x64-setup.exe" }
function Require($ok,$message){ if (!$ok) { throw $message } }
function Entry {
    $rows = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' | Where-Object DisplayName -eq 'Watchman')
    Require ($rows.Count -eq 1) 'Expected one machine uninstall entry'
    $rows[0]
}
function Wait-App($file, $argsList) {
    $process = Start-Process -FilePath $file -ArgumentList $argsList -PassThru
    Require ($process.WaitForExit(120000)) "Timed out: $file"
    Require ($process.ExitCode -eq 0) "Failed ($($process.ExitCode)): $file"
}
$setupInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path $Setup).Path)
Require ($setupInfo.ProductVersion -ceq $version) 'Installer PE ProductVersion is not the candidate version'
$old = Entry
Require ($old.DisplayVersion -eq '1.0.5') 'Baseline missing'
$oldEntryPath = $old.PSPath
$oldInstallPath = $old.InstallLocation.Trim('"')
$app = Join-Path (Split-Path $old.UninstallString.Trim('"')) 'watchman.exe'
Require ((Split-Path $app) -eq $oldInstallPath) 'Baseline install location and uninstaller path disagree'
$profileDir = Join-Path $env:APPDATA 'Watchman'
$native = Join-Path $env:APPDATA 'Watchman.Native'
$null = New-Item $profileDir -ItemType Directory -Force
'{"theme":"light","startOnBoot":true,"unitModeBits":true,"hideGauges":true,"futureField":123}' | Set-Content "$profileDir/settings.json"
# Seed SQLite directly: v1.0.5 itself could import history JSON before upgrade.
python -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('create table daily_traffic(date TEXT PRIMARY KEY, upload_bytes INTEGER NOT NULL DEFAULT 0, download_bytes INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL)'); c.execute('insert into daily_traffic values (?,?,?,?)', ('2025-01-01',707,808,'original')); c.commit(); c.close()" "$profileDir/watchman.db"
'{"records":{}}' | Set-Content "$profileDir/history.json"
'Stay intact' | Set-Content "$profileDir/unknown.txt"
$v1 = Start-Process $app -PassThru
Start-Sleep -Seconds 8
Require (!$v1.HasExited) 'Released v1 is not running'
# The v1 updater uses the passive/update/restart contract; wait for installer, not its restarted child.
if ($ProtocolProbe) { Wait-App $ProtocolProbe '--install' } else { Wait-App $Setup '/P /UPDATE /R' }
Require ($v1.HasExited) 'Old app did not stop'
$new = Entry
Require ($oldEntryPath -eq $new.PSPath -and $new.DisplayName -eq 'Watchman' -and $new.DisplayVersion -eq $version) 'Uninstall identity/version changed'
Require ($new.Publisher -ceq 'Noman Dhoni') 'Installed apps publisher is not exactly Noman Dhoni'
$installPath = $new.InstallLocation.Trim('"')
Require ($installPath -eq $oldInstallPath -and (Split-Path $new.UninstallString.Trim('"')) -eq $oldInstallPath) 'Custom install path was not preserved'
$locationKey = Get-Item 'HKLM:\SOFTWARE\Noman Dhoni\Watchman'
Require ($locationKey.GetValue('') -eq $installPath) 'New publisher install-location key was not saved'
$locationKey.Close()
$legacyLocationKey = Get-Item 'HKLM:\SOFTWARE\blinkeye\Watchman'
Require ($legacyLocationKey.GetValue('') -eq $oldInstallPath) 'Legacy install-location key was not retained for rollback'
$legacyLocationKey.Close()
$defaultInstall = Join-Path $env:ProgramFiles 'Watchman'
if ($defaultInstall -ne $installPath) { Require (!(Test-Path (Join-Path $defaultInstall 'watchman.exe'))) 'Update created a duplicate default-path installation' }
$installedInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($app)
Require ($installedInfo.CompanyName -ceq 'Noman Dhoni') 'Executable CompanyName is not exactly Noman Dhoni'
Require ($installedInfo.FileVersion -ceq $version -and $installedInfo.ProductVersion -ceq $version) 'Executable PE version metadata is not 2.0.1'
Require ((Get-FileHash $app).Hash -eq (Get-Content builds/staged/Watchman.exe.sha256).Trim()) 'Installed executable is not the MSVC build'
$baseline = "$env:RUNNER_TEMP/watchman-v1-baseline/Watchman_1.0.5_x64-setup.exe"
& (Join-Path (Split-Path $app) 'watchman-verify-release.exe') $baseline "$baseline.minisig"
for ($i=0; $i -lt 60 -and !(Test-Path "$native/migration-v2.complete"); $i++) { Start-Sleep 1 }
Require (Test-Path "$native/migration-v2.complete") 'Restart/first launch did not migrate data'
Require (!(Test-Path (Join-Path (Split-Path $app) 'vendor/libre-hardware-monitor/LibreHardwareMonitorLib.dll'))) 'Legacy sensor binaries left installed'
Require (@(Get-Process watchman -ErrorAction Ignore).Count -eq 1) 'Expected one restarted Watchman'
Require ((Get-ItemProperty 'HKCU:\Software\Watchman').UnitModeBits -eq 1) 'Settings were not imported'
Require ((Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').Watchman -match 'watchman.exe" --startup$') 'Startup entry missing or invalid'
foreach ($name in 'settings.json','history.json','unknown.txt','watchman.db') {
    Require ((Get-FileHash "$profileDir/$name").Hash -eq (Get-FileHash "$native/original-v1/$name").Hash) "Original $name changed"
}
python -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); assert c.execute('pragma integrity_check').fetchone()==('ok',); assert c.execute(`"select upload_bytes,download_bytes from daily_traffic where date='2025-01-01'`").fetchone()==(707,808)" "$native/watchman.db"
$links = @(Get-ChildItem "$env:ProgramData/Microsoft/Windows/Start Menu/Programs" -Recurse -Filter 'Watchman.lnk')
Require ($links.Count -eq 1) 'Expected one Start menu shortcut'
Require ((New-Object -ComObject WScript.Shell).CreateShortcut($links[0].FullName).TargetPath -eq $app) 'Wrong shortcut target'
Stop-Process -Name watchman -Force
Wait-Process -Name watchman -Timeout 15 -ErrorAction SilentlyContinue
python "$PSScriptRoot/test_history_policy.py" $app
# Isolate file fixtures; this job owns the entire disposable user and registry.
$real = $env:APPDATA
try {
    $env:APPDATA = Join-Path $env:RUNNER_TEMP 'json-only-profile'
    $older = New-Item "$env:APPDATA/Watchman" -ItemType Directory -Force
    '{"records":{"2020-02-29":{"upload":123,"download":987}}}' | Set-Content "$older/history.json.bak"
    'broken' | Set-Content "$older/history.json"
    # Model a second Windows user with no native preferences (disposable HKCU only).
    Remove-Item 'HKCU:\Software\Watchman' -Recurse
    '{"startOnBoot":false,"notifications":{"warningSettings":{"gpuTempThreshold":91}}}' | Set-Content "$older/settings.json"
    Wait-App $app '--migrate-only'
    Wait-App $app '--migrate-only'
    python -c "import sqlite3,sys; assert sqlite3.connect(sys.argv[1]).execute('select upload_bytes,download_bytes from daily_traffic').fetchall()==[]" "$env:APPDATA/Watchman.Native/watchman.db"
    Require ((Get-ItemProperty 'HKCU:\Software\Watchman').WarningGpuTempThreshold -eq 91) 'Warning threshold lost'
    $quiet = Start-Process $app -ArgumentList '--startup' -PassThru
    for ($i=0; $i -lt 30 -and (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').Watchman; $i++) { Start-Sleep 1 }
    Require (!$quiet.HasExited -and !(Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').Watchman) 'Disabled startup not respected'
    $quiet.Kill(); $quiet.WaitForExit()
    $env:APPDATA = Join-Path $env:RUNNER_TEMP 'damaged-profile'
    $bad = New-Item "$env:APPDATA/Watchman" -ItemType Directory -Force
    'not SQLite' | Set-Content "$bad/watchman.db"
    $before = (Get-FileHash "$bad/watchman.db").Hash
    $failed = Start-Process $app -ArgumentList '--migrate-only' -PassThru -Wait
    Require ($failed.ExitCode -ne 0 -and !(Test-Path "$env:APPDATA/Watchman.Native") -and (Get-FileHash "$bad/watchman.db").Hash -eq $before) 'Damaged history was silently replaced'
} finally { $env:APPDATA = $real }
Wait-App $new.UninstallString.Trim('"') '/S'
for ($i=0; $i -lt 30 -and (Test-Path $app); $i++) {Start-Sleep 1}
Require (!(Test-Path $app) -and (Test-Path "$native/original-v1/history.json")) 'Uninstall removed data or left executable'
# The v1 files were never modified: reinstall the already verified v1 release.
Wait-App "$env:RUNNER_TEMP/watchman-v1-baseline/Watchman_1.0.5_x64-setup.exe" "/S /D=$(Split-Path $app)"
Require ((Entry).DisplayVersion -eq '1.0.5' -and (Test-Path $app)) 'Rollback registration failed'
python -c "import sqlite3,sys; assert (707,808) in sqlite3.connect(sys.argv[1]).execute('select upload_bytes,download_bytes from daily_traffic').fetchall()" "$profileDir/watchman.db"
"Verified rollback installation and original daily totals. Native history retained separately." | Write-Output
