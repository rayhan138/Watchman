# Run AFTER CI has signed the final installer. No signing key needed here.
param([Parameter(Mandatory)][string]$ArtifactDirectory)
$ErrorActionPreference = 'Stop'
$dir = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$version = (Get-Content src-tauri/native-package.json -Raw | ConvertFrom-Json).version
$nativeVersion = [regex]::Match((Get-Content native/version.h -Raw), '(?m)^#define WATCHMAN_VERSION_TEXT "([0-9]+\.[0-9]+\.[0-9]+)"\r?$').Groups[1].Value
if (!$nativeVersion -or $version -cne $nativeVersion) { throw 'Native and installer versions do not match' }
$name = "Watchman_${version}_x64-setup.exe"
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
    version = $version
    notes = "Watchman $version publisher and metadata update."
    pub_date = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    platforms = @{'windows-x86_64' = @{
        signature = $sig
        url = "https://github.com/rayhan138/Watchman/releases/download/v$version/$name"
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
foreach ($asset in $installer, "$installer.sig", (Join-Path $dir 'latest.json')) {
    $hash = (Get-FileHash $asset).Hash
    Write-Output "$hash  $([IO.Path]::GetFileName($asset))"
    if ($env:GITHUB_STEP_SUMMARY) { Add-Content $env:GITHUB_STEP_SUMMARY "- $([IO.Path]::GetFileName($asset)): ``$hash``" }
}
