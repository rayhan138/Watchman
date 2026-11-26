# Disposable hosted-Windows proof that the native registration implementation
# works from a genuinely non-admin account. No UAC/security-policy changes and
# no Watchman startup task/action. The temporary credential is never printed or
# saved; the account, profile, test tasks, and working directory are removed.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string]$ReportPath = 'build/startup-standard-user-tests.txt'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'This account-creation test is restricted to a disposable GitHub Actions Windows runner.' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
$name = 'wmstartup' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$work = Join-Path $env:PUBLIC ('WatchmanStartupTest-' + [guid]::NewGuid().ToString('N'))
$user = $null
$process = $null
$failure = $null
try {
    $password = ConvertTo-SecureString ([guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N') + 'aA1!') -AsPlainText -Force
    $user = New-LocalUser -Name $name -Password $password -AccountNeverExpires -PasswordNeverExpires -Description 'Disposable Watchman startup registration test'
    $usersGroup = Get-LocalGroup -SID 'S-1-5-32-545'
    if (-not (Get-LocalGroupMember -Group $usersGroup.Name | Where-Object { $_.SID -eq $user.SID })) {
        Add-LocalGroupMember -Group $usersGroup.Name -Member $name
    }
    $null = New-Item -ItemType Directory -Path $work
    & icacls.exe $work /grant "*$($user.SID.Value):(OI)(CI)M" | Out-Null
    if ($LASTEXITCODE) { throw 'Cannot grant the temporary standard user access to its isolated test directory.' }
    $copy = Join-Path $work 'StartupRegistrationTest.exe'
    Copy-Item -LiteralPath $Executable -Destination $copy
    $credential = [pscredential]::new("$env:COMPUTERNAME\$name", $password)
    $stdout = Join-Path $work 'stdout.txt'
    $stderr = Join-Path $work 'stderr.txt'
    $process = Start-Process -FilePath $copy -ArgumentList '--require-standard-user' -WorkingDirectory $work -Credential $credential -LoadUserProfile -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    if (-not $process.WaitForExit(60000)) { throw 'Standard-user registration test did not finish within 60 seconds.' }
    # Flush redirected streams before collecting them.
    $process.WaitForExit()
    $lines = @('Isolated standard-user startup registration test', '')
    if (Test-Path $stdout) { $lines += Get-Content -LiteralPath $stdout }
    if (Test-Path $stderr) { $lines += Get-Content -LiteralPath $stderr }
    $lines += "Exit code: $($process.ExitCode)"
    $lines | Set-Content -LiteralPath $ReportPath
    $lines | ForEach-Object { Write-Host $_ }
    if ($process.ExitCode -ne 0) { throw "Native standard-user registration test failed (exit $($process.ExitCode))." }
    if (-not ($lines -match '^Startup registration token: STANDARD \(non-admin, non-elevated\)$')) { throw 'Missing positive standard-user token proof.' }
} catch {
    $failure = $_
    # Exception text may identify the disposable user, never its credential.
    "Standard-user startup registration test failed: $($_.Exception.Message)" | Add-Content -LiteralPath $ReportPath
    if (Test-Path (Join-Path $work 'stdout.txt')) { Get-Content (Join-Path $work 'stdout.txt') | Add-Content -LiteralPath $ReportPath }
    if (Test-Path (Join-Path $work 'stderr.txt')) { Get-Content (Join-Path $work 'stderr.txt') | Add-Content -LiteralPath $ReportPath }
} finally {
    if ($null -ne $process) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; $null = $process.WaitForExit(5000) }
        # The native RAII fixture normally removes its own tasks. On a forced
        # test termination, restrict cleanup to this PID and temporary identity.
        Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object {
            $_.TaskName -like "Watchman Startup Test $($process.Id)-*" -and
            $_.Principal.UserId -in @($user.SID.Value, "$env:COMPUTERNAME\$name", $name)
        } | Unregister-ScheduledTask -Confirm:$false
        $process.Dispose()
    }
    if ($null -ne $user) {
        Get-CimInstance Win32_UserProfile | Where-Object SID -eq $user.SID.Value | Remove-CimInstance
        Remove-LocalUser -SID $user.SID
    }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    $credential = $null
    $password = $null
}
if ($null -ne $failure) { throw $failure }
