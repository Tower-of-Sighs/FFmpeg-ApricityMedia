#requires -Version 7.0

<#
.SYNOPSIS
    Generate Java FFM API bindings from am_ffmpeg.h using jextract.

    Requires:
      - JDK 25+ with jextract (https://github.com/openjdk/jextract)
      - am_ffmpeg.h in the jni/ directory

    Output:
      jextract-gen/cc/sighs/apricitymedia/jni/ffm/ — generated Java sources

    Usage:
      .\gen-ffmapi-bindings.ps1
      .\gen-ffmapi-bindings.ps1 -JextractHome "C:\tools\jextract-25"
#>

param(
    [string]$JextractHome = "",
    [string]$OutputDir = "jextract-gen",
    [string]$HeaderFile = "jni/am_ffmpeg.h",
    [string]$PackageName = "cc.sighs.apricitymedia.jni.ffm"
)

$ErrorActionPreference = 'Stop'

# Locate jextract
if (-not $JextractHome) {
    $candidates = @(
        "${env:JAVA_HOME}/bin/jextract",
        "C:\Program Files\jextract-25\bin\jextract",
        "C:\tools\jextract-25\bin\jextract"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $JextractHome = (Get-Item $c).Directory.Parent.FullName; break }
    }
}

$jextract = if ($JextractHome) {
    Join-Path $JextractHome "bin/jextract"
} else {
    "jextract"
}

if (-not (Get-Command $jextract -ErrorAction SilentlyContinue)) {
    Write-Error "jextract not found. Set -JextractHome or add to PATH."
    exit 1
}

$header = Join-Path $PSScriptRoot $HeaderFile
if (-not (Test-Path $header)) {
    Write-Error "Header not found: $header"
    exit 1
}

$outDir = Join-Path $PSScriptRoot $OutputDir
$null = New-Item -ItemType Directory -Path $outDir -Force

Write-Host "=== Generating FFM API bindings via jextract ===" -ForegroundColor Cyan
Write-Host "  Header:  $header"
Write-Host "  Output:  $outDir"
Write-Host "  Package: $PackageName"
Write-Host "  jextract: $jextract"

# jextract generates Java source files that use the FFM API (Linker, Arena, etc.)
# to call the native C functions exported by am_ffmpeg.
& $jextract `
    --output $outDir `
    --target-package $PackageName `
    --library am_ffmpeg `
    --header-class-name AmFfmpeg `
    -I (Join-Path $PSScriptRoot "jni") `
    $header

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBindings generated in: $outDir" -ForegroundColor Green
    Write-Host "Import into your project and use like:" -ForegroundColor Gray
    Write-Host ""
    Write-Host '  try (Arena arena = Arena.ofConfined()) {' -ForegroundColor Gray
    Write-Host '      long decoder = AmFfmpeg.am_video_open(arena.allocateFrom("video.mp4"), ...);' -ForegroundColor Gray
    Write-Host '      ...' -ForegroundColor Gray
    Write-Host '  }' -ForegroundColor Gray
} else {
    Write-Error "jextract failed (exit code $LASTEXITCODE)"
}
