# Optional API-readable copy of compressed review evidence. Normal full-resolution
# evidence always lives in the Actions artifact. This also works on a previously
# downloaded artifact, independently of an aborted/failed desktop driver.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Directory = (Resolve-Path $Directory).Path
$transport = Join-Path $env:RUNNER_TEMP ('watchman-evidence-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $transport
try {
    foreach ($pattern in @('*.json', '*.jsonl', '*.csv', '*error*.txt', '*-events.txt', '*stderr*.txt', 'startup-recovery.txt', 'startup-task-definition.txt', '*tests.txt', 'report.md')) {
        Get-ChildItem $Directory -Filter $pattern | Copy-Item -Destination $transport
    }
    $encoder = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    $parameters = [Drawing.Imaging.EncoderParameters]::new(1)
    $parameters.Param[0] = [Drawing.Imaging.EncoderParameter]::new([Drawing.Imaging.Encoder]::Quality, [long]48)
    try {
        Get-ChildItem $Directory -Filter '*.png' -Recurse | ForEach-Object {
            $bitmap = [Drawing.Bitmap]::new($_.FullName)
            try { $bitmap.Save((Join-Path $transport ($_.BaseName + '.jpg')), $encoder, $parameters) }
            finally { $bitmap.Dispose() }
        }
    } finally { $parameters.Dispose() }
    $zip = Join-Path $env:RUNNER_TEMP ('watchman-evidence-' + [guid]::NewGuid().ToString('N') + '.zip')
    Compress-Archive -Path (Join-Path $transport '*') -DestinationPath $zip
    $base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($zip))
    Remove-Item $zip
    $chunkSize = 58000
    $chunks = [int][Math]::Ceiling($base64.Length / $chunkSize)
    if ($chunks -gt 40) { throw "Compressed evidence requires $chunks chunks (limit 40). The normal artifact is unaffected." }
    # Workflow-command notices are truncated to 4 KiB and ten per step. The
    # Checks REST API supports full annotation messages (up to 50 per request).
    if (-not $env:GH_TOKEN) { throw 'The evidence publisher needs the workflow checks token.' }
    $annotations = @()
    for ($i = 0; $i -lt $chunks; ++$i) {
        $chunk = $base64.Substring($i * $chunkSize, [Math]::Min($chunkSize, $base64.Length - $i * $chunkSize))
        $annotations += @{
            path = 'tests/windows_smoke_test.ps1'; start_line = 1; end_line = 1
            annotation_level = 'notice'; title = "Compressed review evidence $($i + 1)/$chunks"
            message = "WATCHMAN_EVIDENCE_$($i + 1)_OF_${chunks}:$chunk"
        }
    }
    $body = @{
        name = 'Watchman smoke review evidence'; head_sha = $env:GITHUB_SHA
        status = 'completed'; conclusion = 'neutral'
        details_url = "https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
        output = @{
            title = 'Compressed smoke screenshots and reports (not a test result)'
            summary = "API-readable copy of the normal Watchman-windows-smoke artifact. $chunks compressed chunks. The build/smoke check, not this neutral evidence check, determines success."
            annotations = $annotations
        }
    } | ConvertTo-Json -Depth 8 -Compress
    $headers = @{ Authorization = "Bearer $env:GH_TOKEN"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $check = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$env:GITHUB_REPOSITORY/check-runs" -Headers $headers -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes($body))
    Write-Host "::notice::Smoke review evidence check ID: $($check.id) ($chunks chunks)."
} finally { Remove-Item $transport -Recurse -Force }
