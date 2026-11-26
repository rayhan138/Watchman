function Get-WatchmanRunCommand {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    if ($null -eq $key) { return $null }
    try {
        if ($key.GetValueNames() -notcontains 'Watchman') { return $null }
        return [string]$key.GetValue('Watchman')
    } finally { $key.Close() }
}

function Get-WatchmanStartupTask {
    param([string]$Sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
    $service = New-Object -ComObject 'Schedule.Service'
    $null = $service.Connect()
    $folder = $service.GetFolder('\')
    try { return $folder.GetTask("Watchman Sign-in $Sid") }
    catch {
        $exception = $_.Exception
        while ($null -ne $exception) {
            if ($exception.HResult -in @(-2147024894, -2147024893)) { return $null }
            $exception = $exception.InnerException
        }
        throw
    }
}

function Test-WatchmanStartupTask {
    param([Parameter(Mandatory)][string]$Executable)
    $task = Get-WatchmanStartupTask
    if ($null -eq $task) { return $false }

    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $definition = $task.Definition
    $principal = $definition.Principal
    $principalUser = [string]$principal.UserId
    $samePrincipal = $principalUser -eq $sid
    if (-not $samePrincipal) {
        try {
            $samePrincipal = [Security.Principal.NTAccount]::new($principalUser).Translate(
                [Security.Principal.SecurityIdentifier]).Value -eq $sid
        } catch { $samePrincipal = $false }
    }
    if (-not $samePrincipal -or $definition.RegistrationInfo.Source -cne 'app.blinkeye.watchman.startup.v1' -or
        $principal.LogonType -ne 3 -or $principal.RunLevel -ne 0 -or
        [string]$principal.Id -cne 'WatchmanUser' -or [string]$principal.GroupId) { return $false }

    $triggers = $definition.Triggers
    if ($triggers.Count -ne 1) { return $false }
    $trigger = $triggers.Item(1)
    $triggerUser = [string]$trigger.UserId
    $sameTriggerUser = $triggerUser -eq $sid
    if (-not $sameTriggerUser) {
        try {
            $sameTriggerUser = [Security.Principal.NTAccount]::new($triggerUser).Translate(
                [Security.Principal.SecurityIdentifier]).Value -eq $sid
        } catch { $sameTriggerUser = $false }
    }
    if ($trigger.Type -ne 9 -or -not $trigger.Enabled -or -not $sameTriggerUser -or
        [string]$trigger.StartBoundary -or [string]$trigger.EndBoundary -or
        ([string]$trigger.Delay) -notin @('', 'PT0S')) { return $false }
    $repetition = $trigger.Repetition
    if (([string]$repetition.Interval) -notin @('', 'PT0S') -or
        ([string]$repetition.Duration) -notin @('', 'PT0S')) { return $false }

    $actions = $definition.Actions
    if ($actions.Count -ne 1 -or [string]$actions.Context -cne 'WatchmanUser') { return $false }
    $action = $actions.Item(1)
    if ($action.Type -ne 0 -or
        -not [string]::Equals([string]$action.Path, $Executable, [StringComparison]::OrdinalIgnoreCase) -or
        [string]$action.Arguments -cne '--startup' -or
        -not [string]::Equals([string]$action.WorkingDirectory, (Split-Path -LiteralPath $Executable), [StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }

    $settings = $definition.Settings
    $idle = $settings.IdleSettings
    return [bool]$task.Enabled -and
        [bool]$settings.Enabled -and
        -not [bool]$settings.Hidden -and
        [bool]$settings.AllowDemandStart -and
        -not [bool]$settings.AllowHardTerminate -and
        -not [bool]$settings.DisallowStartIfOnBatteries -and
        -not [bool]$settings.StopIfGoingOnBatteries -and
        -not [bool]$settings.RunOnlyIfNetworkAvailable -and
        -not [bool]$settings.RunOnlyIfIdle -and
        -not [bool]$settings.StartWhenAvailable -and
        -not [bool]$settings.WakeToRun -and
        [int]$settings.MultipleInstances -eq 2 -and
        [int]$settings.Priority -eq 6 -and
        [string]$settings.ExecutionTimeLimit -ceq 'PT0S' -and
        -not [bool]$idle.StopOnIdleEnd -and
        -not [bool]$idle.RestartOnIdle
}

function Assert-WatchmanStartupTask {
    param([Parameter(Mandatory)][string]$Executable)
    if (-not (Test-WatchmanStartupTask -Executable $Executable)) {
        throw "Expected one verified current-user Watchman sign-in task targeting '$Executable'."
    }
    if ($null -ne (Get-WatchmanRunCommand)) {
        throw 'The legacy Watchman Run value remained after Task Scheduler migration.'
    }
}

function Assert-WatchmanStartupAbsent {
    if ($null -ne (Get-WatchmanStartupTask)) { throw 'A current-user Watchman sign-in task remained.' }
    if ($null -ne (Get-WatchmanRunCommand)) { throw 'The current-user Watchman Run value remained.' }
}

Export-ModuleMember -Function Get-WatchmanRunCommand, Get-WatchmanStartupTask, Test-WatchmanStartupTask, Assert-WatchmanStartupTask, Assert-WatchmanStartupAbsent
