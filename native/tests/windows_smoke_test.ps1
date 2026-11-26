# Actual EXE/desktop + production-engine smoke test. Intentionally NOT a unit
# test: requires an unlocked, disposable Windows GitHub Actions runner. It edits
# and restores that runner's HKCU preferences, creates a tray icon and generates
# bounded HTTPS traffic. Never point it at a personal Windows desktop/profile.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeProbe,
    [string]$OutputDirectory = 'smoke-results',
    [switch]$LiveSpeedTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Run this desktop smoke test only on a disposable GitHub Actions Windows runner.' }
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Path (Join-Path $PSScriptRoot 'windows_smoke_native.cs')
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$Executable = (Resolve-Path $Executable).Path
$RuntimeProbe = (Resolve-Path $RuntimeProbe).Path
if (Test-Path $OutputDirectory) { throw 'Use a new smoke output directory; existing evidence is never overwritten.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$shots = Join-Path $OutputDirectory 'screenshots'
$null = New-Item -ItemType Directory -Path $shots
$records = [Collections.Generic.List[object]]::new()
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
$script:main = [IntPtr]::Zero
$script:app = $null
$script:dark = $false
$settingsPath = 'HKCU:\Software\Watchman'
$runPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$themePath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$originalAppdata = $env:APPDATA
$originalLocalAppdata = $env:LOCALAPPDATA
$startupLog = Join-Path $OutputDirectory 'local-appdata\Watchman\startup-recovery.log'
$originalDisposable = $env:WATCHMAN_SMOKE_DISPOSABLE
$settingsBackup = Join-Path $OutputDirectory 'private-settings-backup.reg'
$hadSettings = Test-Path $settingsPath
$runBackup = $null
$themeBackup = @{}
$failure = $null
$startupTaskName = 'Watchman Sign-in ' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$script:startupFolder = $null
$startupTaskScopeSafe = $false

function Get-StartupTask {
    try { return $script:startupFolder.GetTask($startupTaskName) }
    catch {
        $exception = $_.Exception
        while ($null -ne $exception) {
            if ($exception.HResult -in @(-2147024894, -2147024893)) { return $null } # file/path not found
            $exception = $exception.InnerException
        }
        throw
    }
}
function Read-RunCommand {
    $key = Get-Item -LiteralPath $runPath -ErrorAction SilentlyContinue
    if ($null -eq $key) { return '' }
    try { return $key.GetValue('Watchman', '') } finally { $key.Close() }
}
function Seed-LegacyRun {
    if (-not (Test-Path $runPath)) { $null = New-Item -Path $runPath -Force }
    $null = New-ItemProperty -LiteralPath $runPath -Name 'Watchman' -Value ('"' + $Executable + '" --startup') -PropertyType String -Force
}
function Same-User([string]$Identity, [string]$Sid) {
    if ($Identity -eq $Sid) { return $true }
    try { return ([Security.Principal.NTAccount]::new($Identity).Translate([Security.Principal.SecurityIdentifier]).Value -eq $Sid) }
    catch { return $false }
}

function Check([string]$Name, [bool]$Pass, [string]$Detail = '') {
    $record = [pscustomobject]@{ name = $Name; pass = $Pass; detail = $Detail }
    $records.Add($record)
    $record | ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'desktop-checks.jsonl')
    $state = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host "$state | $Name | $Detail"
}
function Observe([string]$Name, [string]$Detail) {
    $record = [pscustomobject]@{ name = $Name; pass = $null; detail = $Detail }
    $records.Add($record)
    $record | ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'desktop-checks.jsonl')
    Write-Host "NOTE | $Name | $Detail"
}
function Wait-Until([scriptblock]$Condition, [int]$Timeout = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 100
    } while ($watch.ElapsedMilliseconds -lt $Timeout)
    return [bool](& $Condition)
}
function Startup-Events([int]$ProcessId) {
    if (Test-Path $startupLog) {
        Get-Content -LiteralPath $startupLog -ErrorAction SilentlyContinue | Where-Object { $_ -match " pid=$ProcessId " }
    }
}
function Record-StartupTiming([int]$ProcessId, [bool]$Quiet = $false) {
    $null = Wait-Until { @(Startup-Events $ProcessId | Where-Object { $_ -match ' ui message-loop-ready ' }).Count -gt 0 } 3000
    # The first measurement is normally displayed on the independent 1Hz tick.
    # Wait for its marker without opening a quiet dashboard or imposing a speed claim.
    $null = Wait-Until { @(Startup-Events $ProcessId | Where-Object { $_ -match ' backend first-measurement ' }).Count -gt 0 } 5000
    $events = @(Startup-Events $ProcessId)
    $launch = @($events | Where-Object { $_ -match ' process (quiet|interactive)-launch ' } | Select-Object -First 1)
    if ($launch.Count -eq 0) { Check 'wWinMain-entry timing marker is available' $false; return }
    $start = [long](($launch[0] -split ' ')[0])
    $timing = [ordered]@{ pid = $ProcessId; quiet = $Quiet; entry_windows_uptime_ms = $start }
    foreach ($phase in @(@('widget_first_frame_ms', ' widget first-frame '), @('message_loop_ms', ' ui message-loop-ready '), @('first_measurement_ms', ' backend first-measurement '))) {
        $line = @($events | Where-Object { $_ -match $phase[1] } | Select-Object -First 1)
        $timing[$phase[0]] = if ($line.Count) { [long](($line[0] -split ' ')[0]) - $start } else { $null }
    }
    $timing | ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'startup-timings.jsonl')
    Observe 'Measured wWinMain-entry-to-widget timing (not OS process creation or Windows sign-in)' ($timing | ConvertTo-Json -Compress)
    if ($Quiet) {
        Check 'Quiet startup never attempts dashboard D2D/fonts before an interactive restore' (@($events | Where-Object { $_ -match ' main-graphics ' }).Count -eq 0)
        Check 'Quiet startup records its hidden launch intent' (@($events | Where-Object { $_ -match ' window quiet-start-hidden ' }).Count -eq 1)
    }
}
function Read-Setting([string]$Name, $Default = $null) {
    $key = Get-Item -LiteralPath $settingsPath -ErrorAction SilentlyContinue
    if ($null -eq $key) { return $Default }
    try { return $key.GetValue($Name, $Default) } finally { $key.Close() }
}
function Write-Setting([string]$Name, [int]$Value) {
    $null = New-ItemProperty -LiteralPath $settingsPath -Name $Name -Value $Value -PropertyType DWord -Force
}
function Dimensions {
    $rect = [WatchmanSmokeNative]::ClientBounds($script:main)
    return [pscustomobject]@{ Width = $rect.Right - $rect.Left; Height = $rect.Bottom - $rect.Top }
}
function Click([int]$X, [int]$Y, [bool]$Double = $false) {
    [WatchmanSmokeNative]::Click($script:main, $X, $Y, $Double)
    Start-Sleep -Milliseconds 180
}
function Key([int]$Code) { [WatchmanSmokeNative]::Key($script:main, $Code); Start-Sleep -Milliseconds 220 }
function Wheel([int]$Detents) { [WatchmanSmokeNative]::Wheel($script:main, $Detents * 120); Start-Sleep -Milliseconds 350 }
function Save-Jpeg($Bitmap, [string]$Path) {
    $encoder = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    $parameters = [Drawing.Imaging.EncoderParameters]::new(1)
    $parameters.Param[0] = [Drawing.Imaging.EncoderParameter]::new([Drawing.Imaging.Encoder]::Quality, [long]58)
    try { $Bitmap.Save($Path, $encoder, $parameters) } finally { $parameters.Dispose() }
}
function Capture([string]$Name, [IntPtr]$Window = $script:main) {
    if ($Window -eq [IntPtr]::Zero) {
        $screen = [Windows.Forms.SystemInformation]::VirtualScreen
        $rect = [pscustomobject]@{ Left = $screen.Left; Top = $screen.Top; Right = $screen.Right; Bottom = $screen.Bottom }
    } else {
        $rect = [WatchmanSmokeNative]::Bounds($Window)
    }
    [WatchmanSmokeNative]::SetCursorPos(1, 1) | Out-Null
    [WatchmanSmokeNative]::InvalidateRect($script:main, [IntPtr]::Zero, $false) | Out-Null
    Start-Sleep -Milliseconds 450
    [WatchmanSmokeNative]::DwmFlush() | Out-Null
    $bitmap = [Drawing.Bitmap]::new(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size, [Drawing.CopyPixelOperation]::SourceCopy)
        $path = Join-Path $shots "$Name.png"
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        Save-Jpeg $bitmap (Join-Path $shots "$Name.jpg")
        $colors = [Collections.Generic.HashSet[int]]::new()
        for ($y = 3; $y -lt $bitmap.Height; $y += 7) {
            for ($x = 3; $x -lt $bitmap.Width; $x += 7) { $null = $colors.Add($bitmap.GetPixel($x, $y).ToArgb()) }
        }
        if ($bitmap.Height -gt 100) { Check "Rendered pixels: $Name" ($colors.Count -gt 50) "unique sampled colors=$($colors.Count), size=$($bitmap.Width)x$($bitmap.Height)" }
        return $path
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Assert-Tab([int]$Index) {
    $d = Dimensions
    $nominal = @(116, 72, 78, 100, 82)
    $scale = [Math]::Min(1.0, ($d.Width - 36.0) / 448.0)
    $left = 10.0
    for ($i = 0; $i -lt $Index; ++$i) { $left += $nominal[$i] * $scale + 4 }
    $bounds = [WatchmanSmokeNative]::ClientBounds($script:main)
    $bitmap = [Drawing.Bitmap]::new(1, 1)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen(($bounds.Left + [int]($left + $nominal[$Index] * $scale / 2)), ($bounds.Top + 55), 0, 0, $bitmap.Size)
        $pixel = $bitmap.GetPixel(0, 0)
        $brightness = ($pixel.R + $pixel.G + $pixel.B) / 3
        $selected = if ($script:dark) { $brightness -gt 170 } else { $brightness -lt 80 }
        Check "Tab $Index visibly selected" $selected "selection pixel brightness=$brightness"
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Select-Tab([int]$Index) {
    $d = Dimensions
    $nominal = @(116, 72, 78, 100, 82)
    $scale = [Math]::Min(1.0, ($d.Width - 36.0) / 448.0)
    $left = 10.0
    for ($i = 0; $i -lt $Index; ++$i) { $left += $nominal[$i] * $scale + 4 }
    Click ([int]($left + $nominal[$Index] * $scale / 2)) 65
    Start-Sleep -Milliseconds 350
    Assert-Tab $Index
}
function Modal {
    $d = Dimensions
    $width = [Math]::Min(500, [Math]::Max(360, $d.Width - 20))
    $height = [Math]::Min(740, [Math]::Max(460, $d.Height - 20))
    $mx = ($d.Width - $width) / 2
    $my = ($d.Height - $height) / 2
    return [pscustomobject]@{ X = $mx; Y = $my; Width = $width; Height = $height; Left = $mx + 150; Right = $mx + $width - 18 }
}
function Open-Settings { $d = Dimensions; Click ($d.Width - 152) 24 }
function Open-SpeedConsent {
    Wheel 30 # A previous run may have brought its result card into view.
    $d = Dimensions
    Click ($d.Width - [Math]::Max(16, [Math]::Min(20, $d.Width * 0.035)) - 58) 578
}
function Close-Settings { $m = Modal; Click ($m.X + $m.Width - 27) ($m.Y + 27) }
function Section([int]$Index) { $m = Modal; Click ($m.X + 55) ($m.Y + 106 + 52 * $Index) }
function Save-Settings { $m = Modal; Click ($m.Right - 40) ($m.Y + $m.Height - 31) }
# Coordinates mirror the shared floating layout; its native geometry/font tests
# cover the minimum modal width. Two 52px detents reveal the whole Text card.
function Floating-Text-Controls([int]$Scroll = 104) {
    $m = Modal
    $left = $m.X + 144
    $width = ($m.Right - $left - 36 - 16) / 3
    return [pscustomobject]@{
        Light = $left + 18 + $width / 2
        Regular = $left + 18 + ($width + 8) + $width / 2
        Bold = $left + 18 + 2 * ($width + 8) + $width / 2
        WeightY = $m.Y + 446 - $Scroll
        Plus = $m.Right - 38
        SizeY = $m.Y + 504 - $Scroll
    }
}
function Set-Floating-ValuesColor([byte]$Red, [byte]$Green, [byte]$Blue, [string]$Stage = 'settings') {
    # Five swatches begin at y=768. Eight detents place all of them in view.
    $m = Modal
    [WatchmanSmokeNative]::BeginClick($script:main, ($m.Right - 54), ($m.Y + 793 - 416))
    if (-not (Wait-Until { [WatchmanSmokeNative]::IsColorDialogReady([WatchmanSmokeNative]::Find('#32770', $script:app.Id)) } 5000)) {
        throw 'The actual Windows color picker did not initialize visible, readable RGB edits.'
    }
    $dialog = [WatchmanSmokeNative]::Find('#32770', $script:app.Id)
    $fields = [WatchmanSmokeNative]::PrepareRgb($dialog, $Red, $Green, $Blue)
    Check "Native color picker contains the chosen RGB values ($Stage)" ($fields -eq "$Red,$Green,$Blue") "actual=$fields"
    $null = Capture "17e-floating-native-color-$Stage" $dialog
    [WatchmanSmokeNative]::AcceptColor($dialog)
    if (-not (Wait-Until { -not [WatchmanSmokeNative]::IsWindow($dialog) } 5000)) { throw 'The native color picker did not close.' }
    Start-Sleep -Milliseconds 250
}
function Open-Popup([IntPtr]$Owner, [bool]$Tray = $false) {
    [WatchmanSmokeNative]::SetCursorPos(150, 150) | Out-Null
    if ($Tray) { $null = [WatchmanSmokeNative]::PostMessage($Owner, 32780, [IntPtr]::Zero, [IntPtr]0x205) }
    else { $null = [WatchmanSmokeNative]::PostMessage($Owner, 0x7B, [IntPtr]::Zero, [WatchmanSmokeNative]::Pack(150, 150)) }
    if (-not (Wait-Until { [WatchmanSmokeNative]::Find('#32768', 0) -ne [IntPtr]::Zero } 5000)) { throw 'Native popup did not open.' }
    $popup = [WatchmanSmokeNative]::Find('#32768', 0)
    $menu = [WatchmanSmokeNative]::Send($popup, 0x1E1, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($menu -eq [IntPtr]::Zero) { throw 'Could not inspect the actual native popup menu.' }
    $labels = @()
    for ($i = 0; $i -lt [WatchmanSmokeNative]::GetMenuItemCount($menu); ++$i) { $labels += [WatchmanSmokeNative]::MenuText($menu, $i) }
    return [pscustomobject]@{ Window = $popup; Menu = $menu; Owner = $Owner; Labels = $labels }
}
function Choose-Popup($Popup, [string]$Label) {
    $index = [Array]::IndexOf([string[]]$Popup.Labels, $Label)
    if ($index -lt 0) { [WatchmanSmokeNative]::EscapeMenu(); throw "Missing native menu item '$Label': $($Popup.Labels -join '; ')" }
    $rect = New-Object 'WatchmanSmokeNative+Rect'
    if (-not [WatchmanSmokeNative]::GetMenuItemRect($Popup.Owner, $Popup.Menu, $index, [ref]$rect)) { throw 'Native menu item bounds unavailable.' }
    [WatchmanSmokeNative]::ActualClick(($rect.Left + $rect.Right) / 2, ($rect.Top + $rect.Bottom) / 2, $false)
    Start-Sleep -Milliseconds 400
}
function Open-Submenu($Popup, [string]$Label) {
    $index = [Array]::IndexOf([string[]]$Popup.Labels, $Label)
    if ($index -lt 0) { [WatchmanSmokeNative]::EscapeMenu(); throw "Missing submenu '$Label'" }
    $menu = [WatchmanSmokeNative]::GetSubMenu($Popup.Menu, $index)
    if ($menu -eq [IntPtr]::Zero) { throw "Submenu '$Label' is not a real native menu" }
    $rect = New-Object 'WatchmanSmokeNative+Rect'
    if (-not [WatchmanSmokeNative]::GetMenuItemRect($Popup.Owner, $Popup.Menu, $index, [ref]$rect)) { throw 'Submenu parent bounds unavailable.' }
    [WatchmanSmokeNative]::ActualClick(($rect.Left + $rect.Right) / 2, ($rect.Top + $rect.Bottom) / 2, $false)
    Start-Sleep -Milliseconds 300
    $labels = @()
    for ($i = 0; $i -lt [WatchmanSmokeNative]::GetMenuItemCount($menu); ++$i) { $labels += [WatchmanSmokeNative]::MenuText($menu, $i) }
    return [pscustomobject]@{ Menu = $menu; Owner = $Popup.Owner; Labels = $labels }
}
function Start-App([string[]]$LaunchArguments = @()) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.WorkingDirectory = Split-Path $Executable
    foreach ($argument in $LaunchArguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $null = $process.Handle # Retain the native handle so fast exits retain ExitCode.
    $processes.Add($process)
    return $process
}
function Find-Main($Process) {
    if (-not (Wait-Until { $Process.HasExited -or [WatchmanSmokeNative]::Find('WatchmanWindowClass', $Process.Id) -ne [IntPtr]::Zero } 30000)) { throw 'Main window did not appear in 30 seconds.' }
    if ($Process.HasExited) { throw "Watchman exited during startup, code $($Process.ExitCode)." }
    $script:main = [WatchmanSmokeNative]::Find('WatchmanWindowClass', $Process.Id)
    $null = [WatchmanSmokeNative]::Send($script:main, 0, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Stop-App($Process, [IntPtr]$Window) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $null = [WatchmanSmokeNative]::PostMessage($Window, 32782, [IntPtr]::Zero, [IntPtr]::Zero)
    $exited = $Process.WaitForExit(12000)
    Check 'Explicit Exit stops the actual process' ($exited -and $Process.ExitCode -eq 0) "finished=$exited, code=$($Process.ExitCode), pid=$($Process.Id), exit_ms=$($watch.ElapsedMilliseconds)"
    if (-not $exited) { $Process.Kill($true); $Process.WaitForExit() }
}

try {
    if (Get-Process Watchman -ErrorAction SilentlyContinue) { throw 'A pre-existing Watchman process is running. Refusing to interfere.' }
    $startupService = New-Object -ComObject 'Schedule.Service'
    $startupService.Connect()
    $script:startupFolder = $startupService.GetFolder('\')
    if ($null -ne (Get-StartupTask)) { throw 'A pre-existing Watchman sign-in task exists. Refusing to change it.' }
    $startupTaskScopeSafe = $true
    if ($hadSettings) { & reg.exe export 'HKCU\Software\Watchman' $settingsBackup /y | Out-Null; if ($LASTEXITCODE) { throw 'Registry backup failed.' } }
    if (Test-Path $runPath) {
        $key = Get-Item $runPath
        try { if ($key.GetValueNames() -contains 'Watchman') { $runBackup = @{ Value = $key.GetValue('Watchman'); Kind = $key.GetValueKind('Watchman') } } } finally { $key.Close() }
    }
    if (-not (Test-Path $themePath)) { $null = New-Item -Path $themePath }
    $key = Get-Item $themePath
    try {
        foreach ($name in @('SystemUsesLightTheme', 'AppsUseLightTheme')) {
            if ($key.GetValueNames() -contains $name) { $themeBackup[$name] = @{ Value = $key.GetValue($name); Kind = $key.GetValueKind($name) } }
            else { $themeBackup[$name] = $null }
        }
    } finally { $key.Close() }
    if ($hadSettings) { Remove-Item $settingsPath -Recurse -Force }
    $null = New-Item $settingsPath -Force
    Write-Setting 'Autostart' 0
    Write-Setting 'Theme' 1
    foreach ($name in @('SystemUsesLightTheme', 'AppsUseLightTheme')) { $null = New-ItemProperty $themePath -Name $name -Value 1 -PropertyType DWord -Force }
    [WatchmanSmokeNative]::BroadcastTheme()
    $installed = [Drawing.Text.InstalledFontCollection]::new()
    try { $matchingFonts = @($installed.Families.Name | Where-Object { $_ -match '^(Inter|Nunito Sans|JetBrains Mono)' }); Observe 'Preinstalled Watchman fonts' ($matchingFonts -join ', ') } finally { $installed.Dispose() }
    $os = Get-CimInstance Win32_OperatingSystem
    Observe 'Environment' "$($os.Caption), build $($os.BuildNumber), runner=$env:RUNNER_OS/$env:RUNNER_ARCH, interactive=$([Environment]::UserInteractive), session=$([Diagnostics.Process]::GetCurrentProcess().SessionId)"
    if ([WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -eq [IntPtr]::Zero) {
        Start-Process explorer.exe | Out-Null
        $null = Wait-Until { [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -ne [IntPtr]::Zero } 10000
    }
    $portable = Join-Path $OutputDirectory 'portable path with spaces'
    $null = New-Item -ItemType Directory -Path $portable
    Copy-Item $Executable (Join-Path $portable 'Watchman.exe')
    $Executable = Join-Path $portable 'Watchman.exe'
    $env:APPDATA = Join-Path $OutputDirectory 'appdata'
    $null = New-Item -ItemType Directory -Path $env:APPDATA
    $env:LOCALAPPDATA = Join-Path $OutputDirectory 'local-appdata'
    $null = New-Item -ItemType Directory -Path $env:LOCALAPPDATA
    $env:WATCHMAN_SMOKE_DISPOSABLE = '1'
    & $RuntimeProbe --seed-profiles $env:APPDATA | Set-Content (Join-Path $OutputDirectory 'profile-seed.jsonl')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot seed isolated native/legacy history profiles.' }
    $legacyDb = Join-Path $env:APPDATA 'Watchman\watchman.db'
    $legacyHash = (Get-FileHash -Algorithm SHA256 $legacyDb).Hash
    $archiveDb = Join-Path $env:APPDATA 'Watchman.Native\original-v1\watchman.db'
    $archiveHash = (Get-FileHash -Algorithm SHA256 $archiveDb).Hash
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $script:app = Start-App
    Find-Main $script:app
    Check 'Portable EXE starts and responds without font/asset sidecars' ([WatchmanSmokeNative]::IsWindowVisible($script:main)) "startup_ms=$($watch.ElapsedMilliseconds), files=$(@(Get-ChildItem $portable).Count)"
    Record-StartupTiming $script:app.Id
    $caption = [WatchmanSmokeNative]::Caption($script:main)
    Check 'Unicode main window retains its full native caption' ([WatchmanSmokeNative]::IsWindowUnicode($script:main) -and $caption -ceq 'WATCHMAN') "caption=$caption"
    [WatchmanSmokeNative]::SetForegroundWindow($script:main) | Out-Null
    $null = Capture '01-welcome-light-1'
    Check 'Opening the first-run tour does not mark it completed' ((Read-Setting 'WelcomeTourVersion' 0) -eq 0)
    Key 39; $null = Capture '02-welcome-light-2'
    Key 37; Key 39; Key 39; $null = Capture '03-welcome-light-3'
    Key 39
    Check 'Completing all three tour pages persists per-user completion' ((Read-Setting 'WelcomeTourVersion' 0) -eq 1)
    Select-Tab 0
    $null = Capture '04-dashboard-light'
    Wheel -12; $null = Capture '04b-dashboard-hardware-light'; Wheel 12
    Select-Tab 1; $null = Capture '05-data-light'
    Wheel -5; $null = Capture '06-data-light-scrolled'
    Select-Tab 2; $null = Capture '07-apps-light'
    Select-Tab 3; $null = Capture '08-network-light'
    Open-SpeedConsent
    $null = Capture '09-speed-consent'
    Key 27
    $null = Capture '10-speed-consent-dismissed'
    Select-Tab 4; $null = Capture '11-tools-light'

    Open-Settings
    $null = Capture '12-settings-general-light'
    $m = Modal
    $oldUnit = Read-Setting 'UnitModeBits' 0
    Click ($m.Right - 40) ($m.Y + 256)
    Check 'Unsaved Settings do not change persisted preferences' ((Read-Setting 'UnitModeBits' 0) -eq $oldUnit)
    Click ($m.X + $m.Width - 99) ($m.Y + 33)
    $null = Capture '13-settings-quick-tour'
    Key 27
    Save-Settings
    Check 'Quick tour preserves the unsaved Settings draft' ((Read-Setting 'UnitModeBits' 0) -eq (1 - $oldUnit))
    Click ($m.Right - 40) ($m.Y + 256)
    Click ($m.Left + 130) ($m.Y + $m.Height - 31)
    Save-Settings
    Check 'Undo discards draft changes' ((Read-Setting 'UnitModeBits' 0) -eq (1 - $oldUnit))
    Seed-LegacyRun # Exercise old Run -> verified task migration through the real UI.
    Click ($m.Right - 40) ($m.Y + 202)
    Save-Settings
    $registered = Wait-Until { $null -ne (Get-StartupTask) -and (Read-RunCommand) -eq '' } 10000
    Check 'Saved startup On migrates the old Run entry to one verified sign-in task' $registered
    if (-not $registered) { throw 'Sign-in task registration did not complete; inspect startup-recovery.txt.' }
    $startupTask = Get-StartupTask
    $definition = $startupTask.Definition
    $principal = $definition.Principal
    $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    Check 'Startup task is interactive/current-user/least-privileged, never highest' ((Same-User $principal.UserId $currentSid) -and $principal.LogonType -eq 3 -and $principal.RunLevel -eq 0)
    $trigger = $definition.Triggers.Item(1)
    # Scheduler can omit the zero-delay default and expose a null BSTR over
    # IDispatch. Normalize null to empty; it is not an added trigger delay.
    $triggerDelay = [string]$trigger.Delay
    $triggerUserMatches = Same-User $trigger.UserId $currentSid
    Check 'Startup task has one current-user sign-in trigger with no added delay' ($definition.Triggers.Count -eq 1 -and $trigger.Type -eq 9 -and $triggerUserMatches -and $triggerDelay -in @('', 'PT0S') -and $trigger.Enabled) "count=$($definition.Triggers.Count), type=$($trigger.Type), current_user=$triggerUserMatches, delay='$triggerDelay', enabled=$($trigger.Enabled)"
    $action = $definition.Actions.Item(1)
    Check 'Task action keeps a spaced portable executable separate from quiet arguments' ($definition.Actions.Count -eq 1 -and $action.Type -eq 0 -and $action.Path -eq $Executable -and $action.Arguments -eq '--startup' -and $action.WorkingDirectory -eq (Split-Path $Executable))
    $taskSettings = $definition.Settings
    Check 'Startup task adds no battery/network/idle gate or runtime limit' (-not $taskSettings.DisallowStartIfOnBatteries -and -not $taskSettings.StopIfGoingOnBatteries -and -not $taskSettings.RunOnlyIfNetworkAvailable -and -not $taskSettings.RunOnlyIfIdle -and $taskSettings.ExecutionTimeLimit -eq 'PT0S' -and -not $taskSettings.AllowHardTerminate -and -not $taskSettings.WakeToRun)
    $definition.XmlText | Set-Content (Join-Path $OutputDirectory 'startup-task-definition.txt')
    # Demand execution is not a sign-in measurement. With the guarded original
    # still running it must be a quiet duplicate and never open another DB.
    $null = $startupTask.Run($null)
    $demandExited = Wait-Until { $task = Get-StartupTask; $task.LastTaskResult -eq 0 -and $task.LastRunTime.Year -ge 2020 -and $task.State -ne 4 } 10000
    Check 'Task Scheduler can execute the actual spaced-path quiet action without duplicating the app' ($demandExited -and -not $script:app.HasExited -and @(Get-Process Watchman -ErrorAction SilentlyContinue).Count -eq 1)
    Click ($m.Right - 40) ($m.Y + 202)
    Save-Settings
    $removed = Wait-Until { $null -eq (Get-StartupTask) -and (Read-RunCommand) -eq '' } 10000
    Check 'Saved startup Off removes both methods without stopping monitoring' ($removed -and -not $script:app.HasExited)
    # Theme IDs are part of the persisted compatibility contract (Dark/Light/Auto).
    $choiceWidth = ($m.Right - $m.Left - 40 - 16) / 3
    Click ($m.Left + 20 + 2 * ($choiceWidth + 8) + $choiceWidth / 2) ($m.Y + 518)
    Save-Settings
    Check 'Auto theme keeps persisted ID 2' ((Read-Setting 'Theme') -eq 2)
    Section 1; $null = Capture '14-settings-widgets-light'
    # The floating-enable row follows the three taskbar-mode choices.
    Click ($m.Right - 40) ($m.Y + 463)
    Save-Settings
    $floating = [WatchmanSmokeNative]::Find('WatchmanFloatingMonitorClass', $script:app.Id)
    Check 'Saved floating-monitor toggle shows its real window' (($floating -ne [IntPtr]::Zero) -and [WatchmanSmokeNative]::IsWindowVisible($floating))
    Section 2; $null = Capture '15-settings-warnings-light'
    Wheel -20; $null = Capture '16-settings-warnings-light-bottom'
    Section 3; $null = Capture '17-settings-floating-light'
    $cpuBefore = Read-Setting 'FloatingMonitorCpu' 1
    $memoryBefore = Read-Setting 'FloatingMonitorMemory' 1
    Click ($m.Right - 40) ($m.Y + 223)
    Click ($m.Right - 40) ($m.Y + 271)
    Check 'CPU/memory settings edit a draft until Save Changes' ((Read-Setting 'FloatingMonitorCpu' 1) -eq $cpuBefore -and (Read-Setting 'FloatingMonitorMemory' 1) -eq $memoryBefore)
    Click ($m.Left + 130) ($m.Y + $m.Height - 31); Save-Settings
    Check 'Undo restores both floating reading choices' ((Read-Setting 'FloatingMonitorCpu') -eq $cpuBefore -and (Read-Setting 'FloatingMonitorMemory') -eq $memoryBefore)
    Click ($m.Right - 40) ($m.Y + 223); Save-Settings
    Check 'CPU and memory can be saved independently in Floating settings' ((Read-Setting 'FloatingMonitorCpu') -eq 0 -and (Read-Setting 'FloatingMonitorMemory') -eq 1)
    Click ($m.Right - 40) ($m.Y + 271); Save-Settings
    $networkBounds = [WatchmanSmokeNative]::Bounds($floating)
    Check 'Disabling both settings restores the original compact network strip' ((Read-Setting 'FloatingMonitorCpu') -eq 0 -and (Read-Setting 'FloatingMonitorMemory') -eq 0 -and ($networkBounds.Bottom - $networkBounds.Top) -eq 28)
    Click ($m.Left + 40) ($m.Y + $m.Height - 31)
    Check 'Floating Defaults also remains an unsaved draft' ((Read-Setting 'FloatingMonitorCpu') -eq 0 -and (Read-Setting 'FloatingMonitorMemory') -eq 0)
    Save-Settings
    Check 'Floating Defaults restores CPU and memory without changing unrelated preferences' ((Read-Setting 'FloatingMonitorCpu') -eq 1 -and (Read-Setting 'FloatingMonitorMemory') -eq 1 -and (Read-Setting 'UnitModeBits') -eq (1 - $oldUnit))
    Click ($m.Right - 40) ($m.Y + 223)
    Close-Settings; Open-Settings; Section 3; Save-Settings
    Check 'Closing Settings discards an unsaved CPU visibility change' ((Read-Setting 'FloatingMonitorCpu') -eq 1)
    Wheel -2
    $text = Floating-Text-Controls
    Check 'An absent old text-weight preference becomes Light without changing the saved size' ((Read-Setting 'FloatingMonitorTextWeight') -eq 300 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12)
    $null = Capture '17a-settings-floating-text-light'
    Click $text.Bold $text.WeightY; Click $text.Plus $text.SizeY
    Check 'Weight and size edits remain unsaved drafts' ((Read-Setting 'FloatingMonitorTextWeight') -eq 300 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12)
    Click ($m.Left + 130) ($m.Y + $m.Height - 31); Save-Settings
    Check 'Undo restores both saved typography choices' ((Read-Setting 'FloatingMonitorTextWeight') -eq 300 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12)
    Click $text.Regular $text.WeightY; Save-Settings
    Check 'Regular can be saved independently of text size' ((Read-Setting 'FloatingMonitorTextWeight') -eq 400 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12)
    Click $text.Bold $text.WeightY
    1..4 | ForEach-Object { Click $text.Plus $text.SizeY }
    Save-Settings
    $larger = [WatchmanSmokeNative]::Bounds($floating)
    Check 'Bold and a larger 16px size are real saved choices, resizing the same monitor' ((Read-Setting 'FloatingMonitorTextWeight') -eq 700 -and (Read-Setting 'FloatingMonitorFontSize') -eq 16 -and ($larger.Right - $larger.Left) -eq 251)
    $null = Capture '17d-settings-floating-text-bold'
    Close-Settings; Open-Settings; Section 3; Wheel -2
    Click $text.Light $text.WeightY
    Close-Settings; Open-Settings; Section 3; Wheel -2; Save-Settings
    Check 'Closing Settings discards an unsaved weight change' ((Read-Setting 'FloatingMonitorTextWeight') -eq 700 -and (Read-Setting 'FloatingMonitorFontSize') -eq 16)
    Click ($m.Left + 40) ($m.Y + $m.Height - 31)
    Check 'Typography Defaults is an unsaved draft until Save Changes' ((Read-Setting 'FloatingMonitorTextWeight') -eq 700 -and (Read-Setting 'FloatingMonitorFontSize') -eq 16)
    Save-Settings
    Check 'Floating Defaults restores Light white 12px without changing unrelated units' ((Read-Setting 'FloatingMonitorTextWeight') -eq 300 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12 -and (Read-Setting 'UnitModeBits') -eq (1 - $oldUnit))
    Wheel -6; $null = Capture '17b-settings-floating-colors-light'
    $oldColor = Read-Setting 'FloatingTextColor'
    Set-Floating-ValuesColor 255 0 0
    Check 'The real native color picker edits only the draft' ((Read-Setting 'FloatingTextColor') -eq $oldColor)
    Save-Settings
    Check 'A chosen red value color saves without forcing Bold or changing the size' ((Read-Setting 'FloatingTextColor') -eq 255 -and (Read-Setting 'FloatingMonitorTextWeight') -eq 300 -and (Read-Setting 'FloatingMonitorFontSize') -eq 12) "color=$(Read-Setting 'FloatingTextColor'), weight=$(Read-Setting 'FloatingMonitorTextWeight'), size=$(Read-Setting 'FloatingMonitorFontSize')"
    Close-Settings
    $null = Capture '29e-floating-red-light' $floating
    Choose-Popup (Open-Popup $floating) 'Transparent background'
    $null = Capture '29f-floating-red-transparent' $floating
    Choose-Popup (Open-Popup $floating) 'Transparent background'
    Open-Settings; Section 3; Wheel -2
    Click $text.Bold $text.WeightY
    Click $text.Plus $text.SizeY; Click $text.Plus $text.SizeY; Save-Settings
    Check 'Custom colors survive a later weight and size change' ((Read-Setting 'FloatingTextColor') -eq 255 -and (Read-Setting 'FloatingMonitorTextWeight') -eq 700 -and (Read-Setting 'FloatingMonitorFontSize') -eq 14) "color=$(Read-Setting 'FloatingTextColor'), weight=$(Read-Setting 'FloatingMonitorTextWeight'), size=$(Read-Setting 'FloatingMonitorFontSize')"
    $null = Capture '17c-settings-floating-text-red-bold'
    Close-Settings
    $null = Capture '29g-floating-red-bold' $floating
    Open-Settings; Section 3
    # Defaults resets floating readings/appearance, not unrelated settings.
    Click ($m.Left + 40) ($m.Y + $m.Height - 31); Save-Settings
    Check 'Floating defaults save the default Light 12px text' ((Read-Setting 'FloatingMonitorFontSize') -eq 12 -and (Read-Setting 'FloatingMonitorTextWeight') -eq 300)
    Wheel -20; $null = Capture '18-settings-floating-light-bottom'
    Close-Settings
    $d = Dimensions
    Click ($d.Width - 112) 24
    $script:dark = $true
    Check 'Quick theme switches Auto/light to explicit Dark ID 0' ((Read-Setting 'Theme') -eq 0)
    Click ($d.Width - 112) 24 $true
    $script:dark = $false
    Check 'Second half of a rapid theme double-click also works' ((Read-Setting 'Theme') -eq 1)
    Click ($d.Width - 112) 24
    $script:dark = $true
    Select-Tab 0; $null = Capture '19-dashboard-dark'
    Wheel -12; $null = Capture '19b-dashboard-hardware-dark'; Wheel 12
    Key 112; $null = Capture '20-welcome-dark-1'
    Key 39; $null = Capture '21-welcome-dark-2'
    Key 39; $null = Capture '22-welcome-dark-3'
    Key 27
    Select-Tab 1; $null = Capture '23-data-dark'
    Select-Tab 2; $null = Capture '24-apps-dark'
    Select-Tab 3; $null = Capture '25-network-dark'
    Open-SpeedConsent; Key 13; $null = Capture '25b-speed-consent-dark'; Key 27
    Select-Tab 4; $null = Capture '26-tools-dark'
    Open-Settings; Section 2; $null = Capture '27-settings-warnings-dark'
    Wheel -20; $null = Capture '28-settings-warnings-dark-bottom'
    Close-Settings

    if ($floating -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($floating)) {
        $bounds = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Floating default geometry includes the CPU/memory row below UP/DN' (($bounds.Right - $bounds.Left) -eq 188 -and ($bounds.Bottom - $bounds.Top) -eq 56)
        $null = Capture '29-floating-monitor' $floating
        $popup = Open-Popup $floating
        Observe 'Actual floating context menu' ($popup.Labels -join ' | ')
        Check 'Floating menu exposes both independent CPU/memory shortcuts and existing controls' (($popup.Labels -contains 'Lock position') -and ($popup.Labels -contains 'Click-through') -and ($popup.Labels -contains 'Show CPU usage') -and ($popup.Labels -contains 'Show memory usage'))
        [WatchmanSmokeNative]::EscapeMenu(); Start-Sleep -Milliseconds 200
        Choose-Popup (Open-Popup $floating) 'Show CPU usage'
        $bounds = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Floating shortcut can show memory without CPU' ((Read-Setting 'FloatingMonitorCpu') -eq 0 -and (Read-Setting 'FloatingMonitorMemory') -eq 1 -and ($bounds.Bottom - $bounds.Top) -eq 56)
        $null = Capture '29b-floating-memory-only' $floating
        Choose-Popup (Open-Popup $floating) 'Show memory usage'
        $bounds = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Floating shortcuts remove the unused hardware row when both are hidden' (($bounds.Bottom - $bounds.Top) -eq 28)
        $null = Capture '29c-floating-network-only' $floating
        Choose-Popup (Open-Popup $floating) 'Show CPU usage'
        $bounds = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Floating shortcut can show CPU without memory' ((Read-Setting 'FloatingMonitorCpu') -eq 1 -and (Read-Setting 'FloatingMonitorMemory') -eq 0 -and ($bounds.Bottom - $bounds.Top) -eq 56)
        $null = Capture '29d-floating-cpu-only' $floating
        Choose-Popup (Open-Popup $floating) 'Show memory usage'
        $weightMenu = Open-Submenu (Open-Popup $floating) 'Text weight'
        $radioStates = @(0..2 | ForEach-Object { [WatchmanSmokeNative]::GetMenuState($weightMenu.Menu, $_, 0x400) -band 8 })
        Check 'Native Text weight menu exposes Light Regular Bold with exactly one selected choice' (($weightMenu.Labels -join '|') -eq 'Light (default)|Regular|Bold' -and $radioStates[0] -eq 8 -and $radioStates[1] -eq 0 -and $radioStates[2] -eq 0)
        Choose-Popup $weightMenu 'Regular'
        Check 'Native Regular weight shortcut saves immediately' ((Read-Setting 'FloatingMonitorTextWeight') -eq 400)
        Choose-Popup (Open-Submenu (Open-Popup $floating) 'Text weight') 'Bold'
        Check 'Native Bold weight shortcut saves immediately' ((Read-Setting 'FloatingMonitorTextWeight') -eq 700)
        Choose-Popup (Open-Submenu (Open-Popup $floating) 'Text weight') 'Light (default)'
        Check 'Native Light weight shortcut restores the thin default' ((Read-Setting 'FloatingMonitorTextWeight') -eq 300)
        $before = [WatchmanSmokeNative]::Bounds($floating)
        [WatchmanSmokeNative]::Drag($floating, -90, -80)
        $after = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Real mouse dragging moves the floating strip' ([Math]::Abs($after.Left - $before.Left) -gt 20 -and [Math]::Abs($after.Top - $before.Top) -gt 20)
        Check 'Dragged floating position is saved' ((Read-Setting 'FloatingMonitorX') -eq $after.Left -and (Read-Setting 'FloatingMonitorY') -eq $after.Top)
        Choose-Popup (Open-Popup $floating) 'Lock position'
        $before = [WatchmanSmokeNative]::Bounds($floating)
        [WatchmanSmokeNative]::Drag($floating, -80, -60)
        $after = [WatchmanSmokeNative]::Bounds($floating)
        Check 'Lock position prevents real mouse dragging' ($before.Left -eq $after.Left -and $before.Top -eq $after.Top -and (Read-Setting 'FloatingMonitorLocked') -eq 1)
        Choose-Popup (Open-Popup $floating) 'Lock position'
        Choose-Popup (Open-Popup $floating) 'Click-through'
        Check 'Click-through applies the native transparent-input style' (([WatchmanSmokeNative]::GetWindowLongPtr($floating, -20).ToInt64() -band 0x20) -ne 0)
        # Recover via the tray's options command, as an actual click-through strip cannot receive right-clicks.
        Choose-Popup (Open-Popup $script:main $true) 'Floating monitor options...'
        $null = Wait-Until { [WatchmanSmokeNative]::Find('#32768', 0) -ne [IntPtr]::Zero } 3000
        $menuWindow = [WatchmanSmokeNative]::Find('#32768', 0)
        $menu = [WatchmanSmokeNative]::Send($menuWindow, 0x1E1, [IntPtr]::Zero, [IntPtr]::Zero)
        $labels = @(); for ($i = 0; $i -lt [WatchmanSmokeNative]::GetMenuItemCount($menu); ++$i) { $labels += [WatchmanSmokeNative]::MenuText($menu, $i) }
        Choose-Popup ([pscustomobject]@{ Owner = $script:main; Menu = $menu; Labels = $labels }) 'Click-through'
        Check 'Tray options recover a click-through floating monitor' (([WatchmanSmokeNative]::GetWindowLongPtr($floating, -20).ToInt64() -band 0x20) -eq 0)
        Select-Tab 0
        [WatchmanSmokeNative]::Click($floating, 30, 14, $true)
        Start-Sleep -Milliseconds 400
        Assert-Tab 1
        Check 'Floating double-click opens Data without moving the strip' ([WatchmanSmokeNative]::Bounds($floating).Left -eq $after.Left)
    }

    $widget = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
    if ($widget -eq [IntPtr]::Zero -or -not [WatchmanSmokeNative]::IsWindowVisible($widget)) {
        Observe 'Taskbar placement retry' ([WatchmanSmokeNative]::WidenDisposableDesktop())
        $null = Wait-Until {
            $widget = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
            [WatchmanSmokeNative]::IsWindowVisible($widget)
        } 6000
        $widget = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
    }
    if ($widget -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($widget)) {
        $widgetParent = [WatchmanSmokeNative]::GetParent($widget)
        Check 'Taskbar widget is embedded, not a top-level overlay' ($widgetParent -ne [IntPtr]::Zero)
        $null = Capture '30-taskbar-light' $widget
        foreach ($name in @('SystemUsesLightTheme', 'AppsUseLightTheme')) { $null = New-ItemProperty $themePath -Name $name -Value 0 -PropertyType DWord -Force }
        [WatchmanSmokeNative]::BroadcastTheme(); Start-Sleep -Milliseconds 1200
        $null = Capture '31-taskbar-dark' $widget
        Select-Tab 0
        $rect = [WatchmanSmokeNative]::Bounds($widget)
        [WatchmanSmokeNative]::ActualClick(($rect.Left + $rect.Right) / 2, ($rect.Top + $rect.Bottom) / 2, $true)
        Start-Sleep -Milliseconds 700
        Assert-Tab 1
    } else {
        Observe 'Taskbar limitation' 'No visible embedded widget on this Windows Server desktop. Taskbar visibility, actual spacing and mouse-hook integration are NOT verified.'
        $null = Capture '30-desktop-taskbar-unavailable' ([IntPtr]::Zero)
    }

    # A real Explorer restart on the disposable desktop, not a posted fake
    # broadcast. Never terminate Explorer outside the wrapper's safety gate.
    $events = @(Startup-Events $script:app.Id)
    Check 'Actual backend reports readiness and a first measurement' (
        @($events | Where-Object { $_ -match ' backend ready ' }).Count -gt 0 -and
        @($events | Where-Object { $_ -match ' backend first-measurement ' }).Count -gt 0)
    $shellWindow = [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null)
    if ($shellWindow -ne [IntPtr]::Zero) {
        [uint32]$shellOwner = 0
        $null = [WatchmanSmokeNative]::GetWindowThreadProcessId($shellWindow, [ref]$shellOwner)
        $shellProcess = [Diagnostics.Process]::GetProcessById($shellOwner)
        if ($shellProcess.ProcessName -ne 'explorer' -or $shellProcess.SessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId) {
            throw 'Refusing to restart an unexpected shell or another Windows session.'
        }
        $backendStarts = @($events | Where-Object { $_ -match ' backend starting ' }).Count
        $presentedBefore = @($events | Where-Object { $_ -match ' graphics (gdi|composition)-presented ' }).Count
        Stop-Process -Id $shellOwner -Force
        if (-not (Wait-Until { [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -ne [IntPtr]::Zero } 5000)) {
            Start-Process explorer.exe | Out-Null
        }
        $shellRestored = Wait-Until { [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -ne [IntPtr]::Zero } 20000
        Check 'Windows recreates the real taskbar after Explorer termination' $shellRestored
        Check 'Explorer restart retains the same Watchman process' (-not $script:app.HasExited -and [WatchmanSmokeNative]::IsWindow($script:main))
        $null = [WatchmanSmokeNative]::Send($script:main, 0, [IntPtr]::Zero, [IntPtr]::Zero)
        $widgetRecovered = Wait-Until {
            $candidate = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
            $candidate -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($candidate) -and
                [WatchmanSmokeNative]::GetParent($candidate) -ne [IntPtr]::Zero
        } 15000
        $events = @(Startup-Events $script:app.Id)
        Check 'Explorer recovery does not restart backend acquisition' (
            @($events | Where-Object { $_ -match ' backend starting ' }).Count -eq $backendStarts)
        if ($widgetRecovered) {
            $frameRecovered = Wait-Until {
                @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' graphics (gdi|composition)-presented ' }).Count -gt $presentedBefore
            } 5000
            Check 'Explorer recovery submits a checked frame, not only WM_PAINT' $frameRecovered
            $widget = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
            $null = Capture '31b-widget-after-explorer-restart' $widget
        } else {
            $latestWidgetStatus = @($events | Where-Object { $_ -match ' widget ' } | Select-Object -Last 1)
            $reason = @($latestWidgetStatus | Where-Object { $_ -match ' widget (waiting-for-taskbar|waiting-for-layout|unsupported-taskbar-bounds|no-safe-space) ' })
            Check 'Hidden widget after shell restart has an explicit safe-placement reason' ($reason.Count -gt 0)
            Observe 'Explorer widget coverage limit' ('No visible widget after real shell restart; visible recovery is NOT verified. Last placement reason: ' + ($reason -join ''))
        }
    } else {
        Observe 'Explorer recovery coverage limit' 'No Explorer taskbar is available to restart; real shell recovery is NOT verified.'
    }

    if ($LiveSpeedTest) {
        # One real consented M-Lab attempt on this disposable public runner, never
        # the user's network/profile. External success is reported separately from
        # the deterministic owned-loopback transfer/cancel tests in CTest.
        Select-Tab 3
        $null = [WatchmanSmokeNative]::SetForegroundWindow($script:main)
        Open-SpeedConsent
        $d = Dimensions
        $dialogWidth = [Math]::Min(460, $d.Width - 40)
        $dialogHeight = [Math]::Min(430, $d.Height - 40)
        $dialogX = ($d.Width - $dialogWidth) / 2
        $dialogY = ($d.Height - $dialogHeight) / 2
        $speedBefore = @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' speed-ui ' }).Count
        Click ([int]($dialogX + ($dialogWidth + 112) / 2)) ([int]($dialogY + $dialogHeight - 41))
        $speedClock = [Diagnostics.Stopwatch]::StartNew()
        $maxSpeedUiMs = 0L
        $capturedPhases = @{}
        $speedFinished = $false
        do {
            $response = [Diagnostics.Stopwatch]::StartNew()
            $null = [WatchmanSmokeNative]::Send($script:main, 0, [IntPtr]::Zero, [IntPtr]::Zero)
            $maxSpeedUiMs = [Math]::Max($maxSpeedUiMs, $response.ElapsedMilliseconds)
            $speedEvents = @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' speed-ui ' } | Select-Object -Skip $speedBefore)
            $latestPhase = if ($speedEvents.Count) { [regex]::Match($speedEvents[-1], ' speed-ui ([a-z-]+) ').Groups[1].Value } else { '' }
            if ($latestPhase -in @('locating','download','upload','finishing','failed','completed') -and -not $capturedPhases.ContainsKey($latestPhase)) {
                $null = Capture ('31c-speed-' + $latestPhase)
                $capturedPhases[$latestPhase] = $true
            }
            $speedFinished = $latestPhase -in @('failed','completed','cancelled')
            if (-not $speedFinished) { Start-Sleep -Milliseconds 100 }
        } while (-not $speedFinished -and $speedClock.Elapsed.TotalSeconds -lt 45)
        Check 'Normal EXE displays a speed-test phase after explicit consent' ($speedEvents.Count -gt 0)
        Check 'Normal EXE remains message-responsive during the real speed-test attempt' ($maxSpeedUiMs -lt 1000) "max_WM_NULL_ms=$maxSpeedUiMs"
        if (-not $speedFinished) {
            Open-SpeedConsent # The same button is Cancel while running.
            $speedFinished = Wait-Until { @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' speed-ui (cancelled|failed|completed) ' }).Count -gt 0 } 20000
            Check 'Timed-out external attempt remains cancellable without closing the app' $speedFinished
        }
        if ($capturedPhases.ContainsKey('download') -and $capturedPhases.ContainsKey('upload') -and $capturedPhases.ContainsKey('completed')) {
            Check 'Normal EXE presents both real M-Lab transfer phases and a completion frame' $true
        } else {
            Observe 'External speed-test coverage limit' ('Full real M-Lab download/upload completion NOT verified. Shown phases: ' + ($capturedPhases.Keys -join ', ') + '. Owned-loopback tests validate both actual WinHTTP transfer paths independently.')
        }

        # If the final card contains a partial/estimate notice, inspect the actual
        # diagnostic dialog too. Never treat an estimate as full external success.
        $d = Dimensions
        Click ($d.Width - 80) ($d.Height - 141)
        $null = Capture '31d-speed-result-details'
        Key 27

    } else {
        Observe 'External speed-test coverage limit' 'Not requested. Deterministic real WinHTTP loopback tests run in CTest. Public M-Lab traffic requires workflow live_speed_test=true and publishes runner IP/time/results.'
    }

    [WatchmanSmokeNative]::SetWindowPos($script:main, [IntPtr]::Zero, 100, 60, 460, 650, 0x40) | Out-Null
    $null = Capture '32-compact-dark'
    Open-Settings; Section 2; Wheel -20; $null = Capture '33-compact-warnings-dark'
    Close-Settings
    Select-Tab 3; Open-SpeedConsent; $null = Capture '33b-compact-speed-consent-dark'; Key 27
    $widgetBeforeClose = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
    $hadVisibleWidget = $widgetBeforeClose -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($widgetBeforeClose)
    $floatingBeforeClose = [WatchmanSmokeNative]::Find('WatchmanFloatingMonitorClass', $script:app.Id)
    $hadVisibleFloating = $floatingBeforeClose -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($floatingBeforeClose)
    # Exercise the actual client-drawn cross, not just a synthetic WM_CLOSE.
    $d = Dimensions
    $hiddenFramesBefore = @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' widget measurement-after-dashboard-hide ' }).Count
    $widgetPreferenceBefore = Read-Setting 'Widget' 1
    $floatingPreferenceBefore = Read-Setting 'FloatingMonitor' 0
    Click ($d.Width - 24) 24
    $hidden = Wait-Until { $script:app.HasExited -or -not [WatchmanSmokeNative]::IsWindowVisible($script:main) } 5000
    Check 'Close hides to tray without stopping monitoring' ($hidden -and -not $script:app.HasExited -and -not [WatchmanSmokeNative]::IsWindowVisible($script:main))
    if ($hadVisibleWidget) {
        $keptWidget = Wait-Until {
            $w = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
            $w -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($w) -and [WatchmanSmokeNative]::GetParent($w) -ne [IntPtr]::Zero
        } 5000
        Check 'Closing the dashboard retains the enabled visible taskbar widget' $keptWidget
        if ($keptWidget) {
            $continued = Wait-Until { @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' widget measurement-after-dashboard-hide ' }).Count -gt $hiddenFramesBefore } 6000
            Check 'Backend acquisition and accepted widget frames continue after dashboard Close' $continued
            $null = Capture '33c-taskbar-with-dashboard-closed' ([WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id))
        }
    } else { Observe 'Close-to-tray widget coverage limit' 'Widget was not visible before Close; this case does not prove visible-widget retention.' }
    if ($hadVisibleFloating) {
        Check 'Closing the dashboard retains the enabled floating monitor' ([WatchmanSmokeNative]::IsWindowVisible($floatingBeforeClose))
        $null = Capture '33d-floating-with-dashboard-closed' $floatingBeforeClose
    } else { Observe 'Close-to-tray floating coverage limit' 'Floating monitor was not visible before Close; retention is NOT verified.' }
    Check 'Dashboard Close does not change saved widget preferences' ((Read-Setting 'Widget' 1) -eq $widgetPreferenceBefore -and (Read-Setting 'FloatingMonitor' 0) -eq $floatingPreferenceBefore)
    $duplicate = Start-App
    $exited = $duplicate.WaitForExit(10000)
    $restored = Wait-Until { [WatchmanSmokeNative]::IsWindowVisible($script:main) } 5000
    Check 'Second interactive launch exits and restores the same instance' ($exited -and $duplicate.ExitCode -eq 0 -and $restored -and -not $script:app.HasExited) "duplicate_exited=$exited, code=$($duplicate.ExitCode), restored=$restored, original_exited=$($script:app.HasExited), original_pid=$($script:app.Id), duplicate_pid=$($duplicate.Id)"
    if (-not $exited -or -not $restored) {
        Observe 'Original instance windows after failed activation' ([WatchmanSmokeNative]::DescribeWindows($script:app.Id))
        Observe 'Duplicate instance windows after failed activation' ([WatchmanSmokeNative]::DescribeWindows($duplicate.Id))
        $null = Capture 'duplicate-launch-failure-desktop' ([IntPtr]::Zero)
        if (-not $exited) { $duplicate.Kill($true); $duplicate.WaitForExit() }
    }
    [WatchmanSmokeNative]::ShowWindow($script:main, 6) | Out-Null
    Check 'Minimize uses native minimized state' ([WatchmanSmokeNative]::IsIconic($script:main))
    $duplicate = Start-App
    $exited = $duplicate.WaitForExit(10000)
    $restored = Wait-Until { [WatchmanSmokeNative]::IsWindowVisible($script:main) -and -not [WatchmanSmokeNative]::IsIconic($script:main) } 5000
    Check 'Interactive relaunch restores the same minimized instance' ($exited -and $duplicate.ExitCode -eq 0 -and $restored -and -not $script:app.HasExited) "duplicate_exited=$exited, code=$($duplicate.ExitCode), restored=$restored"
    if (-not $exited) { Observe 'Minimized activation duplicate windows' ([WatchmanSmokeNative]::DescribeWindows($duplicate.Id)); $duplicate.Kill($true); $duplicate.WaitForExit() }
    $floating = [WatchmanSmokeNative]::Find('WatchmanFloatingMonitorClass', $script:app.Id)
    Choose-Popup (Open-Submenu (Open-Popup $floating) 'Text weight') 'Bold'
    Choose-Popup (Open-Submenu (Open-Popup $floating) 'Text size') '14 px'
    Open-Settings; Section 3; Wheel -8
    Set-Floating-ValuesColor 255 0 0 -Stage 'restart'; Save-Settings; Close-Settings
    $oldProcess = $script:app
    Choose-Popup (Open-Popup $script:main $true) 'Restart Watchman'
    $oldExited = $oldProcess.WaitForExit(15000)
    $restarted = Wait-Until {
        $window = [WatchmanSmokeNative]::Find('WatchmanWindowClass', 0)
        if ($window -eq [IntPtr]::Zero) { return $false }
        [uint32]$owner = 0
        $null = [WatchmanSmokeNative]::GetWindowThreadProcessId($window, [ref]$owner)
        return $owner -ne $oldProcess.Id
    } 20000
    Check 'Actual tray Restart replaces the process cleanly' ($oldExited -and $oldProcess.ExitCode -eq 0 -and $restarted)
    if (-not $restarted) { throw 'Restart did not produce a usable replacement.' }
    $script:main = [WatchmanSmokeNative]::Find('WatchmanWindowClass', 0)
    [uint32]$owner = 0; $null = [WatchmanSmokeNative]::GetWindowThreadProcessId($script:main, [ref]$owner)
    $script:app = [Diagnostics.Process]::GetProcessById($owner); $null = $script:app.Handle; $processes.Add($script:app)
    Check 'Replacement retains committed Settings and tour completion' ((Read-Setting 'UnitModeBits' 0) -eq (1 - $oldUnit) -and (Read-Setting 'WelcomeTourVersion' 0) -eq 1)
    $floating = [WatchmanSmokeNative]::Find('WatchmanFloatingMonitorClass', $script:app.Id)
    $weightMenu = Open-Submenu (Open-Popup $floating) 'Text weight'
    $boldSelected = ([WatchmanSmokeNative]::GetMenuState($weightMenu.Menu, 2, 0x400) -band 8) -ne 0
    [WatchmanSmokeNative]::EscapeMenu(); [WatchmanSmokeNative]::EscapeMenu(); Start-Sleep -Milliseconds 150
    $resized = [WatchmanSmokeNative]::Bounds($floating)
    Check 'A real process restart loads Bold 14px and the custom red color' ($boldSelected -and (Read-Setting 'FloatingMonitorTextWeight') -eq 700 -and (Read-Setting 'FloatingMonitorFontSize') -eq 14 -and (Read-Setting 'FloatingTextColor') -eq 255 -and ($resized.Right - $resized.Left) -eq 220) "bold_menu=$boldSelected, color=$(Read-Setting 'FloatingTextColor'), weight=$(Read-Setting 'FloatingMonitorTextWeight'), size=$(Read-Setting 'FloatingMonitorFontSize'), width=$($resized.Right - $resized.Left)"
    $null = Capture '33e-floating-text-after-restart' $floating
    Open-Settings; Section 3
    $m = Modal
    Click ($m.Left + 40) ($m.Y + $m.Height - 31); Save-Settings; Close-Settings
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero

    # Real rapid launches exercise normal shutdown while asynchronous setup
    # may still be in flight. The probe separately blocks setup deterministically.
    for ($quick = 0; $quick -lt 3; ++$quick) {
        $script:app = Start-App @('--startup')
        Find-Main $script:app
        Check "Rapid quiet launch $quick has a responsive hidden window host" (-not $script:app.HasExited -and -not [WatchmanSmokeNative]::IsWindowVisible($script:main))
        Stop-App $script:app $script:main
        $script:main = [IntPtr]::Zero
    }

    # Start the unmodified EXE during a real Explorer interruption on this
    # disposable desktop. Native CTest separately forces blocked/failed tray
    # calls; Explorer may auto-restart too quickly to cause a real add failure.
    $quietShell = [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null)
    if ($quietShell -ne [IntPtr]::Zero) {
        [uint32]$quietShellOwner = 0
        $null = [WatchmanSmokeNative]::GetWindowThreadProcessId($quietShell, [ref]$quietShellOwner)
        $quietShellProcess = [Diagnostics.Process]::GetProcessById($quietShellOwner)
        if ($quietShellProcess.ProcessName -ne 'explorer' -or $quietShellProcess.SessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId) {
            throw 'Refusing to interrupt an unexpected shell or another Windows session.'
        }
        Stop-Process -Id $quietShellOwner -Force
        $script:app = Start-App @('--startup')
        Find-Main $script:app
        Check 'Quiet EXE stays hidden and unfocused while Explorer is restarting' ([WatchmanSmokeNative]::StaysHidden($script:main, 1200))
        if (-not (Wait-Until { [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -ne [IntPtr]::Zero } 3000)) {
            Start-Process explorer.exe | Out-Null
        }
        $quietShellRestored = Wait-Until { [WatchmanSmokeNative]::FindWindow('Shell_TrayWnd', $null) -ne [IntPtr]::Zero } 20000
        Check 'Real taskbar returns during quiet EXE recovery' $quietShellRestored
        $quietTrayReady = Wait-Until { @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' shell tray-ready ' }).Count -gt 0 } 10000
        Check 'Quiet EXE recovers its real tray registration without opening the dashboard' ($quietTrayReady -and [WatchmanSmokeNative]::StaysHidden($script:main, 1200))
        Record-StartupTiming $script:app.Id $true
        $quietEvents = @(Startup-Events $script:app.Id)
        $unavailable = @($quietEvents | Where-Object { $_ -match ' shell tray-unavailable ' }).Count
        Observe 'Real tray-failure coverage during the shell interruption' "tray-unavailable markers=$unavailable. A fast Explorer restart may avoid this race; native CTest deterministically blocks/fails it. This is not a Windows sign-in test."
        $quietWidgetReady = Wait-Until {
            $w = [WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id)
            $w -ne [IntPtr]::Zero -and [WatchmanSmokeNative]::IsWindowVisible($w) -and [WatchmanSmokeNative]::GetParent($w) -ne [IntPtr]::Zero
        } 6000
        if ($quietWidgetReady) {
            $quietFrame = Wait-Until { @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' graphics gdi-presented ' }).Count -gt 0 } 3000
            Check 'Quiet shell recovery accepts an independent first GDI widget frame' $quietFrame
            $null = Capture '34a-quiet-start-recovered-taskbar' ([WatchmanSmokeNative]::Find('WatchmanWidgetClass', $script:app.Id))
        } else {
            $quietReason = @((Startup-Events $script:app.Id) | Where-Object { $_ -match ' widget ' } | Select-Object -Last 1)
            Check 'Unavailable quiet widget has an explicit safe-placement reason' (@($quietReason | Where-Object { $_ -match ' widget (waiting-for-taskbar|waiting-for-layout|unsupported-taskbar-bounds|no-safe-space) ' }).Count -gt 0)
            Observe 'Quiet real-shell widget coverage limit' ('No visible safe gap; visible recovery is NOT verified. ' + ($quietReason -join ''))
        }
        Check 'Quiet shell recovery leaves all dashboard graphics unloaded' (@((Startup-Events $script:app.Id) | Where-Object { $_ -match ' main-graphics ' }).Count -eq 0)
        Stop-App $script:app $script:main
        $script:main = [IntPtr]::Zero
    } else {
        Observe 'Quiet real-shell interruption coverage limit' 'No Explorer taskbar was available; native owned-window tests cover blocked and failed tray calls instead.'
    }

    # A quiet-startup flag is not proof of an actual Windows sign-in.
    # First sign-in-style startup must remain quiet and defer, not consume, the tour.
    Remove-ItemProperty $settingsPath -Name 'WelcomeTourVersion'
    Write-Setting 'Autostart' 1
    Seed-LegacyRun
    $script:app = Start-App @('--startup')
    Find-Main $script:app
    Record-StartupTiming $script:app.Id $true
    $migrated = Wait-Until { @(Startup-Events $script:app.Id | Where-Object { $_ -match ' startup task-ready ' }).Count -eq 1 } 10000
    Check 'Automatic upgrade migrates an enabled legacy Run item only after the UI loop is ready' ($migrated -and $null -ne (Get-StartupTask) -and (Read-RunCommand) -eq '')
    $migrationEvents = @(Startup-Events $script:app.Id)
    $loopLine = @($migrationEvents | Where-Object { $_ -match ' ui message-loop-ready ' } | Select-Object -First 1)
    $startupLine = @($migrationEvents | Where-Object { $_ -match ' startup task-ready ' } | Select-Object -First 1)
    Check 'Registration completion is not an initial widget/tray/message-loop gate' ($loopLine.Count -eq 1 -and $startupLine.Count -eq 1 -and [long](($startupLine[0] -split ' ')[0]) -ge [long](($loopLine[0] -split ' ')[0]))
    Check 'Startup launch stays in the tray and defers onboarding' (-not [WatchmanSmokeNative]::IsWindowVisible($script:main) -and (Read-Setting 'WelcomeTourVersion' 0) -eq 0)
    $duplicate = Start-App @('--startup')
    $duplicateExited = $duplicate.WaitForExit(10000)
    Check 'Duplicate startup launch does not open a window' ($duplicateExited -and -not [WatchmanSmokeNative]::IsWindowVisible($script:main))
    $null = [WatchmanSmokeNative]::Send($script:main, 32781, [IntPtr]::Zero, [IntPtr]::Zero)
    $null = Capture '34-deferred-welcome'
    Check 'First interactive restore still has an incomplete tour' ((Read-Setting 'WelcomeTourVersion' 0) -eq 0)
    Key 27
    Check 'Skipping the deferred tour saves completion' ((Read-Setting 'WelcomeTourVersion' 0) -eq 1)
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero
    Check 'Explicit Exit leaves the enabled sign-in preference registered' ($null -ne (Get-StartupTask))

    $script:app = Start-App @('--startup')
    Find-Main $script:app
    $unchanged = Wait-Until { @(Startup-Events $script:app.Id | Where-Object { $_ -match ' startup task-ready ' }).Count -eq 1 } 10000
    Check 'A subsequent quiet launch does not rewrite a matching task or recreate Run' ($unchanged -and (Read-RunCommand) -eq '' -and @(Startup-Events $script:app.Id | Where-Object { $_ -match ' startup registration-changed ' }).Count -eq 0)
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero

    (Get-StartupTask).Enabled = $false
    $disabledTaskXml = (Get-StartupTask).Xml
    $script:app = Start-App @('--startup') # Deliberate manual launch, not a sign-in trigger.
    Find-Main $script:app
    $externalDisabled = Wait-Until { @(Startup-Events $script:app.Id | Where-Object { $_ -match ' startup externally-disabled ' }).Count -eq 1 } 10000
    Check 'An externally disabled task stays disabled across an ordinary quiet launch' ($externalDisabled -and -not (Get-StartupTask).Enabled -and (Get-StartupTask).Xml -eq $disabledTaskXml -and (Read-RunCommand) -eq '')
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero

    Write-Setting 'Autostart' 0
    $script:app = Start-App @('--startup')
    Find-Main $script:app
    $savedOff = Wait-Until { @(Startup-Events $script:app.Id | Where-Object { $_ -match ' startup disabled ' }).Count -eq 1 } 10000
    Check 'A saved Off survives launch and removes a disabled sign-in task too' ($savedOff -and $null -eq (Get-StartupTask) -and (Read-RunCommand) -eq '')
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero
    $appDb = Join-Path $env:APPDATA 'Watchman.Native\watchman.db'
    Check 'Real EXE reuses the existing isolated native profile' (Test-Path $appDb)
    Check 'New test-session history in the legacy DB is byte-identical after the native-profile fix' ((Get-FileHash -Algorithm SHA256 $legacyDb).Hash -eq $legacyHash)
    Check 'The original-v1 archival DB was not restored or changed' ((Get-FileHash -Algorithm SHA256 $archiveDb).Hash -eq $archiveHash)
    $allEvents = @(Get-Content -LiteralPath $startupLog)
    $malformedEvents = @($allEvents | Where-Object { $_ -and $_ -notmatch '^\d+ pid=\d+ [a-z0-9-]+ [a-z0-9-]+ error=\d+$' })
    Check 'Duplicate launches do not corrupt startup transition records' ($malformedEvents.Count -eq 0) "malformed_records=$($malformedEvents.Count)"
    $updateStarts = @($allEvents | Where-Object { $_ -match ' updates check-started ' })
    $updateStartSafe = $updateStarts.Count -gt 0
    foreach ($start in $updateStarts) {
        $parts = $start -split ' '
        $updatePid = $parts[1]
        $ready = @($allEvents | Where-Object { $_ -match ("^\d+ " + [regex]::Escape($updatePid) + " ui message-loop-ready ") } | Select-Object -First 1)
        if ($ready.Count -ne 1 -or ([long]$parts[0] - [long](($ready[0] -split ' ')[0])) -lt 15000) { $updateStartSafe = $false }
    }
    Check 'Actual EXE defers automatic release checks at least 15 seconds after its own UI-loop marker' $updateStartSafe ("checks_started=" + $updateStarts.Count)
    $updateResults = @($allEvents | Where-Object { $_ -match ' updates (checked|check-failed|available) ' })
    Check 'Actual release-check worker returns a truthful result without blocking desktop smoke' ($updateResults.Count -gt 0) ($updateResults -join '; ')
    Observe 'Update notification delivery limit' 'The actual EXE uses the real official stable channel. It is not fed a fake newer version; Windows notification visibility/activation requires a genuinely newer release on Windows 10/11. Native UI FIXTURE screenshots are separate owned test windows.'
    Check 'Actual EXE reports selecting native history rather than stale legacy history' (@($allEvents | Where-Object { $_ -match ' history native-profile ' }).Count -gt 0)
    & $RuntimeProbe --verify-profiles $env:APPDATA | Set-Content (Join-Path $OutputDirectory 'profile-verification.jsonl')
    Check 'Real EXE preserves native rows without merging independent legacy/JSON/backup history' ($LASTEXITCODE -eq 0)
    if (Test-Path $appDb) {
        & $RuntimeProbe --inspect-db $appDb | Set-Content (Join-Path $OutputDirectory 'application-database.jsonl')
        Check 'Real EXE history database passes SQLite integrity check' ($LASTEXITCODE -eq 0)
    }
} catch {
    $failure = $_
    Check 'Desktop smoke harness completed all scenarios' $false $_.Exception.Message
    $_ | Format-List * -Force | Out-String | Set-Content (Join-Path $OutputDirectory 'desktop-error.txt')
    try { $null = Capture 'failure-desktop' ([IntPtr]::Zero) } catch { Write-Warning $_ }
} finally {
    try {
        [WatchmanSmokeNative]::EscapeMenu()
        foreach ($process in $processes) {
            try { if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit(5000) | Out-Null } } catch { Write-Warning $_ }
        }
        if ($startupTaskScopeSafe) {
            $task = Get-StartupTask
            if ($null -ne $task) {
                if ($task.Definition.RegistrationInfo.Source -ne 'app.blinkeye.watchman.startup.v1' -or
                    -not (Same-User $task.Definition.Principal.UserId ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value))) {
                    throw 'Refusing cleanup of an unowned startup task.'
                }
                $script:startupFolder.DeleteTask($startupTaskName, 0)
            }
            Check 'Disposable smoke leaves no Watchman sign-in task' ($null -eq (Get-StartupTask))
        }
        if (Test-Path $settingsPath) { Remove-Item $settingsPath -Recurse -Force }
        if ($hadSettings -and (Test-Path $settingsBackup)) { & reg.exe import $settingsBackup | Out-Null }
        if ($null -ne $runBackup) {
            $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
            try { $key.SetValue('Watchman', $runBackup.Value, $runBackup.Kind) } finally { $key.Close() }
        } elseif (Test-Path $runPath) { Remove-ItemProperty $runPath -Name Watchman -ErrorAction SilentlyContinue }
        foreach ($name in $themeBackup.Keys) {
            if ($null -eq $themeBackup[$name]) { Remove-ItemProperty $themePath -Name $name -ErrorAction SilentlyContinue }
            else { $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Themes\Personalize', $true); try { $key.SetValue($name, $themeBackup[$name].Value, $themeBackup[$name].Kind) } finally { $key.Close() } }
        }
        [WatchmanSmokeNative]::BroadcastTheme()
        if (Test-Path $settingsBackup) { Remove-Item $settingsBackup -Force }
    } catch {
        Check 'Restore disposable runner preferences' $false $_.Exception.Message
        $_ | Format-List * -Force | Out-String | Set-Content (Join-Path $OutputDirectory 'cleanup-error.txt')
    } finally {
        $env:APPDATA = $originalAppdata
        $env:LOCALAPPDATA = $originalLocalAppdata
        [WatchmanSmokeNative]::RestoreDisplay()
    }
}

# Run backend integration even if an unrelated desktop assertion failed. Do not
# overlap the two engines: their real ETW session has one production owner.
try {
    $env:WATCHMAN_SMOKE_DISPOSABLE = '1'
    $info = [Diagnostics.ProcessStartInfo]::new($RuntimeProbe)
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Environment['LOCALAPPDATA'] = Join-Path $OutputDirectory 'local-appdata'
    $info.ArgumentList.Add('--exercise'); $info.ArgumentList.Add($OutputDirectory)
    $probe = [Diagnostics.Process]::Start($info)
    $stdoutTask = $probe.StandardOutput.ReadToEndAsync()
    $stderrTask = $probe.StandardError.ReadToEndAsync()
    $finished = $probe.WaitForExit(180000)
    if (-not $finished) { $probe.Kill($true); $probe.WaitForExit() }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $stdout | Set-Content (Join-Path $OutputDirectory 'runtime-probe.jsonl')
    $stderr | Set-Content (Join-Path $OutputDirectory 'runtime-probe-stderr.txt')
    foreach ($line in ($stdout -split '\r?\n' | Where-Object { $_.StartsWith('{') })) {
        $event = $line | ConvertFrom-Json
        if ($event.event -eq 'check') { Check "Engine: $($event.name)" $event.pass $event.detail }
        elseif ($event.event -eq 'observation') { Observe $event.name $event.detail }
    }
    Check 'Production-engine integration probe exits without a crash/hang' ($finished -and $probe.ExitCode -eq 0) "finished=$finished, exit=$($probe.ExitCode)"
} catch { Check 'Runtime probe wrapper' $false $_.Exception.Message }
finally { $env:WATCHMAN_SMOKE_DISPOSABLE = $originalDisposable }

try {
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = @(1000, 1001); StartTime = (Get-Date).AddMinutes(-15) } -ErrorAction SilentlyContinue |
        Where-Object Message -match 'Watchman.exe' | Select-Object -Property @('TimeCreated', 'Id', 'Message') |
        Format-List | Out-String | Set-Content (Join-Path $OutputDirectory 'application-crash-events.txt')
} catch { Write-Warning $_ }

if (Test-Path $startupLog) { Copy-Item $startupLog (Join-Path $OutputDirectory 'startup-recovery.txt') }

$failures = @($records | Where-Object { $null -ne $_.pass -and -not $_.pass })
$report = [pscustomobject]@{ executableSha256 = (Get-FileHash $Executable -Algorithm SHA256).Hash; commit = $env:GITHUB_SHA; failures = $failures.Count; checks = $records }
$report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'report.json')
$markdown = @('# Watchman actual-runtime smoke test', '', "Commit: $env:GITHUB_SHA", '', "Failures: $($failures.Count)", '', '| Result | Scenario | Evidence / observation |', '|---|---|---|')
foreach ($record in $records) {
    $result = if ($null -eq $record.pass) { 'NOTE' } elseif ($record.pass) { 'PASS' } else { 'FAIL' }
    $markdown += "| $result | $($record.name.Replace('|', '/')) | $($record.detail.Replace('|', '/')) |"
}
$markdown += @('', 'Screenshots are captured from the real desktop, not generated previews. A nonblank image alone is not a visual-layout pass.', '', 'Limits: hosted VM/Windows Server is not a physical Windows 10/11 PC. UAC approval, sign-in, sleep/resume, multiple monitors, GPU/temperature accuracy, warning-toast delivery and completed public speed-test accuracy need separate evidence.')
$markdown | Set-Content (Join-Path $OutputDirectory 'report.md')
if ($env:GITHUB_STEP_SUMMARY) { $markdown | Add-Content $env:GITHUB_STEP_SUMMARY }
foreach ($failed in ($failures | Select-Object -First 8)) { Write-Host "::error::$($failed.name): $($failed.detail.Replace('%', '%25').Replace("`n", '%0A').Replace("`r", '%0D'))" }

Write-Host "Smoke test finished with $($failures.Count) failure(s). Evidence: $OutputDirectory"
if ($failures.Count -gt 0) { exit 1 }
