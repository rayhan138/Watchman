# Run AFTER CI has signed the final installer. No signing key needed here.
param([Parameter(Mandatory)][string]$ArtifactDirectory)
$ErrorActionPreference = 'Stop'
$dir = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$name = 'Watchman_2.0.0_x64-setup.exe'
$installer = Join-Path $dir $name
$sig = (Get-Content -LiteralPath "$installer.sig" -Raw).Trim()
$temp = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllBytes($temp, [Convert]::FromBase64String($sig))
    cargo run --locked --release --manifest-path "$PSScriptRoot/verify/Cargo.toml" -- $installer $temp
    if ($LASTEXITCODE) { throw 'Refusing to generate manifest: installer signature verification failed' }
} finally {
    Remove-Item -LiteralPath $temp
}
$feed = [ordered]@{
    version = '2.0.0'
    notes = "Update level: optional`n`nMigrate Watchman to the native Windows application."
    pub_date = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    platforms = @{'windows-x86_64' = @{
        signature = $sig
        url = "https://github.com/rayhan138/Watchman/releases/download/v2.0.0/$name"
    }}
}
# Never overwrite even a local pre-existing manifest.
$out = [IO.File]::Open((Join-Path $dir 'latest.json'), 'CreateNew', 'Write', 'None')
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($feed | ConvertTo-Json -Depth 5))
    $out.Write($bytes, 0, $bytes.Length)
} finally {
    $out.Dispose()
}
