param(
  [Parameter(Mandatory = $true)]
  [string]$SensorRoot
)

$ErrorActionPreference = 'Stop'

function Update-HardwareNode {
  param([LibreHardwareMonitor.Hardware.IHardware]$Hardware)

  $Hardware.Update()
  foreach ($subHardware in $Hardware.SubHardware) {
    Update-HardwareNode $subHardware
  }
}

function Add-TemperatureSample {
  param(
    [string]$Bucket,
    [string]$SensorName,
    [double]$Value,
    [hashtable]$Samples
  )

  if ($Value -le 0 -or $Value -gt 150) {
    return
  }

  if (-not $Samples.ContainsKey($Bucket)) {
    $Samples[$Bucket] = New-Object System.Collections.Generic.List[object]
  }

  $Samples[$Bucket].Add([pscustomobject]@{
      Name  = $SensorName
      Value = [math]::Round($Value, 1)
    })
}

function Get-PreferredTemperature {
  param(
    [System.Collections.Generic.List[object]]$Candidates,
    [string[]]$PreferredPatterns
  )

  if (-not $Candidates -or $Candidates.Count -eq 0) {
    return $null
  }

  foreach ($pattern in $PreferredPatterns) {
    $match = $Candidates | Where-Object { $_.Name -match $pattern } | Sort-Object Value -Descending | Select-Object -First 1
    if ($match) {
      return $match.Value
    }
  }

  return ($Candidates | Sort-Object Value -Descending | Select-Object -First 1).Value
}

$libraryPath = Join-Path $SensorRoot 'LibreHardwareMonitorLib.dll'
if (-not (Test-Path $libraryPath)) {
  throw "LibreHardwareMonitor library not found at $libraryPath"
}

[void][System.Reflection.Assembly]::LoadFrom($libraryPath)

$computer = New-Object LibreHardwareMonitor.Hardware.Computer
$computer.IsCpuEnabled = $true
$computer.IsGpuEnabled = $true
$computer.IsStorageEnabled = $true
$computer.IsMotherboardEnabled = $true
$computer.Open()

$samples = @{}

try {
  foreach ($hardware in $computer.Hardware) {
    Update-HardwareNode $hardware

    $nodes = @($hardware) + @($hardware.SubHardware)
    foreach ($node in $nodes) {
      $hardwareType = $node.HardwareType.ToString()
      foreach ($sensor in $node.Sensors) {
        if ($sensor.SensorType.ToString() -ne 'Temperature' -or $null -eq $sensor.Value) {
          continue
        }

        $sensorName = $sensor.Name
        $sensorValue = [double]$sensor.Value

        switch -Regex ($hardwareType) {
          '^Cpu' {
            Add-TemperatureSample -Bucket 'cpu' -SensorName $sensorName -Value $sensorValue -Samples $samples
            break
          }
          '^Gpu' {
            Add-TemperatureSample -Bucket 'gpu' -SensorName $sensorName -Value $sensorValue -Samples $samples
            break
          }
          '^Storage' {
            Add-TemperatureSample -Bucket 'disk' -SensorName $sensorName -Value $sensorValue -Samples $samples
            break
          }
          'Motherboard|SuperIO' {
            Add-TemperatureSample -Bucket 'mainboard' -SensorName $sensorName -Value $sensorValue -Samples $samples
            break
          }
        }
      }
    }
  }
}
finally {
  $computer.Close()
}

$result = [ordered]@{
  cpu       = Get-PreferredTemperature -Candidates $samples['cpu'] -PreferredPatterns @('package', 'tctl', 'tdie', 'cpu')
  gpu       = Get-PreferredTemperature -Candidates $samples['gpu'] -PreferredPatterns @('core', 'hot spot', 'junction', 'gpu')
  disk      = Get-PreferredTemperature -Candidates $samples['disk'] -PreferredPatterns @('temperature', 'composite', 'assembly')
  mainboard = Get-PreferredTemperature -Candidates $samples['mainboard'] -PreferredPatterns @('motherboard', 'system', 'chipset', 'vrm')
}

$result | ConvertTo-Json -Compress
