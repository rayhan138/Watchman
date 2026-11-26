# Only on a fresh disposable Windows runner with the selected public v2 baseline installed.
param(
    [string]$Setup,
    [string]$ProtocolProbe,
    [ValidateSet('2.0.0','2.0.1')][string]$BaselineVersion = '2.0.0'
)
trap { Write-Host "::error::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"; exit 1 }
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$version = (Get-Content src-tauri/native-package.json -Raw | ConvertFrom-Json).version
if (!$Setup) { $Setup = "builds/staged/Watchman_${version}_x64-setup.exe" }
$baselineFolder = if ($BaselineVersion -eq '2.0.0') { 'watchman-v2-baseline' } else { 'watchman-v201-baseline' }
$baselinePublisher = if ($BaselineVersion -eq '2.0.0') { 'blinkeye' } else { 'Noman Dhoni' }
$baseline = Join-Path $env:RUNNER_TEMP "$baselineFolder/Watchman_${BaselineVersion}_x64-setup.exe"
Import-Module "$PSScriptRoot/StartupAcceptance.psm1" -Force
function Require($ok,$message){ if (!$ok) { throw $message } }
function Entry {
    $rows = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' | Where-Object DisplayName -eq 'Watchman')
    Require ($rows.Count -eq 1) 'Expected exactly one machine uninstall entry'
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
Require ($old.DisplayVersion -eq $BaselineVersion -and $old.Publisher -ceq $baselinePublisher) "Genuine v$BaselineVersion baseline is missing"
$oldEntryPath = $old.PSPath
$oldInstallPath = $old.InstallLocation.Trim('"')
$app = Join-Path (Split-Path $old.UninstallString.Trim('"')) 'watchman.exe'
Require ((Split-Path $app) -eq $oldInstallPath) 'Baseline install location and uninstaller path disagree'
$native = Join-Path $env:APPDATA 'Watchman.Native'
$original = Join-Path $native 'original-v1'
$profile = Join-Path $native 'profiles\custom.profile'
$marker = Join-Path $native 'migration-v2.complete'
$null = New-Item $native -ItemType Directory -Force
$null = New-Item $original -ItemType Directory -Force
$null = New-Item (Split-Path $profile) -ItemType Directory -Force
[IO.File]::WriteAllText($marker, "2.0.0`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($profile, "Keep existing native profile data`n", [Text.UTF8Encoding]::new($false))
$jsonFixture = '{"records":{"2025-01-01":{"upload":999,"download":888},"2025-01-02":{"upload":777,"download":666}}}'
[IO.File]::WriteAllText((Join-Path $original 'history.json'), $jsonFixture, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $original 'history.json.bak'), $jsonFixture, [Text.UTF8Encoding]::new($false))
python "$PSScriptRoot/test_native_upgrade.py" seed (Join-Path $native 'watchman.db')
$preservedFiles = @($marker, $profile, (Join-Path $original 'history.json'), (Join-Path $original 'history.json.bak'))
$hashesBefore = @{}
foreach ($file in $preservedFiles) { $hashesBefore[$file] = (Get-FileHash $file).Hash }

# The old native build stored this saved preference and launched through Run.
# v2.0.2 must migrate it to one current-user task, not silently switch startup off.
$settingsKey = 'HKCU:\Software\Watchman'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$null = New-Item $settingsKey -Force
New-ItemProperty $settingsKey -Name Autostart -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty $settingsKey -Name Theme -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty $settingsKey -Name UnitModeBits -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty $settingsKey -Name DataLimitBytes -Value ([long]9876543210) -PropertyType QWord -Force | Out-Null
$null = New-Item $runKey -Force
$startup = "`"$app`" --startup"
New-ItemProperty $runKey -Name Watchman -Value $startup -PropertyType String -Force | Out-Null

$oldProcess = Start-Process $app -PassThru
Start-Sleep -Seconds 8
Require (!$oldProcess.HasExited) "Released v$BaselineVersion is not running"
$settingsBefore = Get-ItemProperty $settingsKey
$startupBefore = Get-WatchmanRunCommand
Require ($settingsBefore.Autostart -eq 1 -and $settingsBefore.Theme -eq 1 -and
    $settingsBefore.UnitModeBits -eq 1 -and $settingsBefore.DataLimitBytes -eq 9876543210) 'Could not seed representative native settings'
Require ($startupBefore -ceq $startup) 'Could not seed the per-user startup command'
Require ($null -eq (Get-WatchmanStartupTask)) 'The disposable baseline already had a new-format sign-in task'

# The staging probe verifies the exact signed candidate and launches it through its real updater path.
if ($ProtocolProbe) { Wait-App $ProtocolProbe '--install' } else { Wait-App $Setup '/P /UPDATE /R' }
Require ($oldProcess.HasExited) "Released v$BaselineVersion process did not stop for update"
$new = Entry
Require ($new.PSPath -eq $oldEntryPath -and $new.DisplayName -eq 'Watchman' -and $new.DisplayVersion -eq $version) 'Uninstall identity/version changed'
Require ($new.Publisher -ceq 'Noman Dhoni') 'Installed apps publisher is not exactly Noman Dhoni'
$installPath = $new.InstallLocation.Trim('"')
Require ($installPath -eq $oldInstallPath -and (Split-Path $new.UninstallString.Trim('"')) -eq $oldInstallPath) 'Custom install path was not preserved'
$locationKey = Get-Item 'HKLM:\SOFTWARE\Noman Dhoni\Watchman'
Require ($locationKey.GetValue('') -eq $installPath) 'New publisher install-location key was not saved'
$locationKey.Close()
if ($BaselineVersion -eq '2.0.0') {
    $legacyLocationKey = Get-Item 'HKLM:\SOFTWARE\blinkeye\Watchman'
    Require ($legacyLocationKey.GetValue('') -eq $oldInstallPath) 'Legacy install-location key was not retained for rollback'
    $legacyLocationKey.Close()
} elseif (Test-Path 'HKLM:\SOFTWARE\blinkeye\Watchman') {
    $legacyLocationKey = Get-Item 'HKLM:\SOFTWARE\blinkeye\Watchman'
    Require ($legacyLocationKey.GetValue('') -eq $oldInstallPath) 'Existing legacy install-location key changed'
    $legacyLocationKey.Close()
}
$defaultInstall = Join-Path $env:ProgramFiles 'Watchman'
if ($defaultInstall -ne $installPath) { Require (!(Test-Path (Join-Path $defaultInstall 'watchman.exe'))) 'Update created a duplicate default-path installation' }
$installedInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($app)
Require ($installedInfo.CompanyName -ceq 'Noman Dhoni') 'Executable CompanyName is not exactly Noman Dhoni'
Require ($installedInfo.FileVersion -ceq $version -and $installedInfo.ProductVersion -ceq $version) 'Executable PE version metadata is not the candidate version'
Require ((Get-FileHash $app).Hash -eq (Get-Content builds/staged/Watchman.exe.sha256).Trim()) 'Installed executable is not the MSVC build'

& (Join-Path (Split-Path $app) 'watchman-verify-release.exe') $baseline "${baseline}.minisig"
Require ($LASTEXITCODE -eq 0) "Could not re-verify the unmodified released v$BaselineVersion baseline with the shipped public key"
for ($i=0; $i -lt 60 -and @(Get-Process watchman -ErrorAction Ignore).Count -ne 1; $i++) { Start-Sleep 1 }
Require (@(Get-Process watchman -ErrorAction Ignore).Count -eq 1) 'Expected exactly one restarted Watchman process'
$registrationReady = $false
for ($i = 0; $i -lt 60; $i++) {
    if ((Test-WatchmanStartupTask -Executable $app) -and $null -eq (Get-WatchmanRunCommand)) {
        $registrationReady = $true
        break
    }
    Start-Sleep 1
}
Require $registrationReady 'Enabled Run startup was not migrated to the verified current-user sign-in task'
Assert-WatchmanStartupTask -Executable $app
$settingsAfter = Get-ItemProperty $settingsKey
Require ($settingsAfter.Autostart -eq 1 -and $settingsAfter.Theme -eq $settingsBefore.Theme -and
    $settingsAfter.UnitModeBits -eq $settingsBefore.UnitModeBits -and
    $settingsAfter.DataLimitBytes -eq 9876543210) 'Native settings changed during the update'
Require ((Get-Content -Raw $marker).Trim() -eq '2.0.0') 'Completed migration marker was rewritten'
foreach ($file in $preservedFiles) { Require ((Get-FileHash $file).Hash -eq $hashesBefore[$file]) "Existing profile or archived JSON changed: $file" }
$links = @(Get-ChildItem "$env:ProgramData/Microsoft/Windows/Start Menu/Programs" -Recurse -Filter 'Watchman.lnk')
Require ($links.Count -eq 1) 'Expected exactly one Start menu shortcut after update'
Require ((New-Object -ComObject WScript.Shell).CreateShortcut($links[0].FullName).TargetPath -eq $app) 'Start menu shortcut points outside the preserved install path'
Stop-Process -Name watchman -Force
Wait-Process -Name watchman -Timeout 15 -ErrorAction SilentlyContinue
python "$PSScriptRoot/test_native_upgrade.py" verify (Join-Path $native 'watchman.db')

# The machine uninstaller must remove this current user's owned task but retain all data.
Wait-App $new.UninstallString.Trim('"') '/S'
for ($i = 0; $i -lt 30 -and (Test-Path $app); $i++) { Start-Sleep 1 }
Require (!(Test-Path $app) -and (Test-Path "$native/watchman.db") -and (Test-Path $marker)) 'Uninstall removed profile data or left the executable'
Assert-WatchmanStartupAbsent
"Verified v$BaselineVersion -> $version in-place update, startup migration, publisher, install identity, custom path, SQLite, settings, profile preservation and uninstall cleanup." | Write-Output
