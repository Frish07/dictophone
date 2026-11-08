param([string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'app'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force $Destination,$cache | Out-Null
$archive = Join-Path $cache 'ffmpeg.zip'
$downloadUrl = 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip'
# Pinned upstream GitHub asset digest, also published at gyan.dev/ffmpeg/builds.
$expected = '60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba'
if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $expected) {
    Invoke-WebRequest -Uri $downloadUrl -OutFile $archive
}
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($actual -ine $expected) {
    throw 'FFmpeg SHA256 mismatch. Delete build/ffmpeg.zip and retry.'
}
$unpacked = Join-Path $cache 'ffmpeg-unpacked'
Expand-Archive -LiteralPath $archive -DestinationPath $unpacked -Force
$encoder = @(Get-ChildItem -LiteralPath $unpacked -Recurse -Filter ffmpeg.exe)
if ($encoder.Count -ne 1) { throw 'Expected exactly one FFmpeg executable.' }
Copy-Item -LiteralPath $encoder[0].FullName -Destination (Join-Path $Destination 'ffmpeg.exe') -Force
$probe = Join-Path $encoder[0].Directory.FullName 'ffprobe.exe'
if (Test-Path -LiteralPath $probe) { Copy-Item -LiteralPath $probe -Destination (Join-Path $Destination 'ffprobe.exe') -Force }
$license = Join-Path $encoder[0].Directory.Parent.FullName 'LICENSE'
if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license -Destination (Join-Path $Destination 'ffmpeg-license.txt') -Force }
@("Source: $downloadUrl", "SHA256: $actual", 'Upstream sources: https://ffmpeg.org/ and https://www.gyan.dev/ffmpeg/builds/') | Set-Content -LiteralPath (Join-Path $Destination 'ffmpeg-build.txt') -Encoding UTF8
Write-Host "FFmpeg verified and installed in $Destination"
