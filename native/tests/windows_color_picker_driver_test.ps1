# Isolate the external smoke driver's interaction with a normal ChooseColorW.
# The dialog runs in a separate process with the same flags as Watchman, never
# writes app preferences, and reports the color returned by the native dialog.
[CmdletBinding()]
param(
    [string]$OutputDirectory = 'smoke-driver-results',
    [switch]$OwnDialog,
    [string]$ResultPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Run only on a disposable GitHub Actions Windows runner.' }
if ($OwnDialog) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeColorDialogOwner {
    [StructLayout(LayoutKind.Sequential)] private struct ChooseColor {
        public uint Size;
        public IntPtr Owner, Instance;
        public uint Result;
        public IntPtr CustomColors;
        public uint Flags;
        public IntPtr CustomData, Hook, Template;
    }
    [DllImport("comdlg32.dll", CharSet=CharSet.Unicode)]
    private static extern bool ChooseColorW(ref ChooseColor value);
    public static string Show() {
        IntPtr custom = Marshal.AllocHGlobal(16 * sizeof(uint));
        try {
            Marshal.Copy(new int[16],0,custom,16);
            ChooseColor value = new ChooseColor();
            value.Size = (uint)Marshal.SizeOf(typeof(ChooseColor));
            value.CustomColors = custom;
            value.Result = 0xFFFFFF;
            value.Flags = 3; // CC_RGBINIT | CC_FULLOPEN, just like Watchman.
            bool accepted = ChooseColorW(ref value);
            return (accepted ? "accepted:" : "cancelled:") + value.Result;
        } finally { Marshal.FreeHGlobal(custom); }
    }
}
'@
    [NativeColorDialogOwner]::Show() | Set-Content -LiteralPath $ResultPath
    exit
}
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'windows_smoke_native.cs')
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $OutputDirectory) { throw 'Use a new driver output directory.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$shots = Join-Path $OutputDirectory 'screenshots'
$null = New-Item -ItemType Directory -Path $shots
$result = Join-Path $OutputDirectory 'native-result.txt'
$child = $null
$dialog = [IntPtr]::Zero
function Capture-Dialog([string]$Name) {
    $rect = [WatchmanSmokeNative]::Bounds($dialog)
    $bitmap = [Drawing.Bitmap]::new(($rect.Right-$rect.Left),($rect.Bottom-$rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left,$rect.Top,0,0,$bitmap.Size)
        $bitmap.Save((Join-Path $shots "$Name.png"),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
    $info.UseShellExecute = $false
    foreach ($argument in @('-NoProfile','-STA','-File',$PSCommandPath,'-OwnDialog','-ResultPath',$result)) {
        $info.ArgumentList.Add($argument)
    }
    $child = [Diagnostics.Process]::Start($info)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ($child.HasExited) { throw "Owned native dialog exited before appearing ($($child.ExitCode))." }
        $dialog = [WatchmanSmokeNative]::Find('#32770',$child.Id)
        if ([WatchmanSmokeNative]::IsColorDialogReady($dialog)) { break }
        Start-Sleep -Milliseconds 100
    } while ($clock.ElapsedMilliseconds -lt 15000)
    if (-not [WatchmanSmokeNative]::IsColorDialogReady($dialog)) { throw 'Owned native color dialog RGB edits did not initialize.' }
    [WatchmanSmokeNative]::DescribeChildren($dialog) | Set-Content (Join-Path $OutputDirectory 'native-controls-before-tests.txt')
    Capture-Dialog 'native-color-before'
    $initial = [WatchmanSmokeNative]::ReadRgb($dialog)
    $samples = @()
    foreach ($choice in @(
        @{ Red = 255; Green = 0; Blue = 0; Expected = '255,0,0' },
        @{ Red = 17; Green = 83; Blue = 201; Expected = '17,83,201' },
        @{ Red = 255; Green = 0; Blue = 0; Expected = '255,0,0' }
    )) {
        $fields = [WatchmanSmokeNative]::PrepareRgb($dialog,$choice.Red,$choice.Green,$choice.Blue)
        $samples += [ordered]@{ expected = $choice.Expected; actual = $fields; pass = ($fields -eq $choice.Expected) }
    }
    [WatchmanSmokeNative]::DescribeChildren($dialog) | Set-Content (Join-Path $OutputDirectory 'native-controls-after-tests.txt')
    Capture-Dialog 'native-color-after'
    [WatchmanSmokeNative]::AcceptColor($dialog)
    if (-not $child.WaitForExit(5000)) { throw 'Native color OK did not close the separate process.' }
    $nativeResult = (Get-Content -LiteralPath $result -Raw).Trim()
    $report = [ordered]@{ initial = $initial; edits = $samples; returned = $nativeResult; child_exit = $child.ExitCode }
    $report | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'driver-report.json')
    if ($initial -ne '255,255,255' -or @($samples | Where-Object { -not $_.pass }).Count -gt 0 -or $nativeResult -ne 'accepted:255' -or $child.ExitCode -ne 0) {
        throw "Native color driver failed: $($report | ConvertTo-Json -Compress)"
    }
    Write-Host 'PASS | External color driver reads back red and the normal ChooseColorW returns RGB(255,0,0).'
} finally {
    foreach ($file in @(Get-ChildItem $OutputDirectory -Filter '*.txt')) {
        Write-Host "--- $($file.Name) ---"
        Get-Content -LiteralPath $file.FullName | ForEach-Object { Write-Host $_ }
    }
    if ($null -ne $child -and -not $child.HasExited) {
        if ($dialog -ne [IntPtr]::Zero) { $null = [WatchmanSmokeNative]::PostMessage($dialog,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
        if (-not $child.WaitForExit(2000)) { $child.Kill($true); $child.WaitForExit() }
    }
}
