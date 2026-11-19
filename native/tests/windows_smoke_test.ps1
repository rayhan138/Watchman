# Actual EXE/desktop + production-engine smoke test. Intentionally NOT a unit
# test: requires an unlocked, disposable Windows GitHub Actions runner. It edits
# and restores that runner's HKCU preferences, creates a tray icon and generates
# bounded HTTPS traffic. Never point it at a personal Windows desktop/profile.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeProbe,
    [string]$OutputDirectory = 'smoke-results'
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
$originalDisposable = $env:WATCHMAN_SMOKE_DISPOSABLE
$settingsBackup = Join-Path $OutputDirectory 'private-settings-backup.reg'
$hadSettings = Test-Path $settingsPath
$runBackup = $null
$themeBackup = @{}
$failure = $null

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
    $d = Dimensions
    Click ($d.Width - [Math]::Max(16, [Math]::Min(20, $d.Width * 0.035)) - 58) 578
}
function Close-Settings { $m = Modal; Click ($m.X + $m.Width - 27) ($m.Y + 27) }
function Section([int]$Index) { $m = Modal; Click ($m.X + 55) ($m.Y + 106 + 52 * $Index) }
function Save-Settings { $m = Modal; Click ($m.Right - 40) ($m.Y + $m.Height - 31) }
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
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $script:app = Start-App
    Find-Main $script:app
    Check 'Portable EXE starts and responds without font/asset sidecars' ([WatchmanSmokeNative]::IsWindowVisible($script:main)) "startup_ms=$($watch.ElapsedMilliseconds), files=$(@(Get-ChildItem $portable).Count)"
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
    Click ($m.Right - 40) ($m.Y + 202)
    Save-Settings
    $runKey = Get-Item $runPath
    try { $registeredStartup = $runKey.GetValue('Watchman', '') } finally { $runKey.Close() }
    Check 'Startup registration quotes the portable path and uses quiet startup' ($registeredStartup -eq ('"' + $Executable + '" --startup'))
    Click ($m.Right - 40) ($m.Y + 202)
    Save-Settings
    $runKey = Get-Item $runPath
    try { $registeredStartup = $runKey.GetValue('Watchman', '') } finally { $runKey.Close() }
    Check 'Turning startup off removes the Run entry' ($registeredStartup -eq '')
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
    Wheel -20; $null = Capture '18-settings-floating-light-bottom'
    # Defaults only resets floating appearance, not unrelated settings.
    Click ($m.Left + 40) ($m.Y + $m.Height - 31); Save-Settings
    Check 'Floating defaults save the default 12px font' ((Read-Setting 'FloatingMonitorFontSize') -eq 12)
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
        Check 'Floating default geometry contains only the compact UP/DN strip' (($bounds.Right - $bounds.Left) -eq 188 -and ($bounds.Bottom - $bounds.Top) -eq 28)
        $null = Capture '29-floating-monitor' $floating
        $popup = Open-Popup $floating
        Observe 'Actual floating context menu' ($popup.Labels -join ' | ')
        Check 'Floating menu has drag/options controls, not CPU/memory modes' (($popup.Labels -contains 'Lock position') -and ($popup.Labels -contains 'Click-through') -and -not (($popup.Labels -join ' ') -match 'CPU|memory'))
        [WatchmanSmokeNative]::EscapeMenu(); Start-Sleep -Milliseconds 200
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
        $null = Wait-Until { [WatchmanSmokeNative]::IsWindowVisible($widget) } 6000
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

    [WatchmanSmokeNative]::SetWindowPos($script:main, [IntPtr]::Zero, 100, 60, 460, 650, 0x40) | Out-Null
    $null = Capture '32-compact-dark'
    Open-Settings; Section 2; Wheel -20; $null = Capture '33-compact-warnings-dark'
    Close-Settings
    Select-Tab 3; Open-SpeedConsent; $null = Capture '33b-compact-speed-consent-dark'; Key 27
    $null = [WatchmanSmokeNative]::Send($script:main, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    Check 'Close hides to tray without stopping monitoring' (-not $script:app.HasExited -and -not [WatchmanSmokeNative]::IsWindowVisible($script:main))
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
    Stop-App $script:app $script:main
    $script:main = [IntPtr]::Zero

    # First sign-in-style startup must remain quiet and defer, not consume, the tour.
    Remove-ItemProperty $settingsPath -Name 'WelcomeTourVersion'
    $script:app = Start-App @('--startup')
    Find-Main $script:app
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
    $appDb = Join-Path $env:APPDATA 'Watchman.Native\watchman.db'
    Check 'Real EXE creates persistent history at isolated APPDATA' (Test-Path $appDb)
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
