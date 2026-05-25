#requires -Version 7.0

<#
.SYNOPSIS
    Package CI artifacts into MC-version-specific zips.

    CI produces one zip per platform (include/ + lib/ + bin/). This script
    strips everything except the runtime files and splits by Minecraft version.

    Notes:
      - MC 1.20.1 / 1.21.1: uses a JNI wrapper shared library + FFmpeg shared libs
      - MC 26.1: uses Java 25 FFM API (no JNI wrapper) + FFmpeg shared libs

    Usage:
      .\package-dist.ps1 -ArtifactsDir libs -OutputDir dist-mc
      .\package-dist.ps1 -ArtifactsDir libs -OnlyPlatforms windows-x64
#>

param(
    [Parameter(Mandatory)]
    [string]$ArtifactsDir,

    [string]$OutputDir = "dist-mc",

    # Optional filter; when set, only process these platform names.
    [string[]]$OnlyPlatforms
)

$ErrorActionPreference = 'Stop'

# ---- Platform definitions ----
# FfBase = FFmpeg shared lib names (without extension)
# FfInBin = $true -> look in bin/; $false -> look in lib/
$platformDefs = @(
    [pscustomobject]@{ Name = "windows-x64";  Ext = "dll";   JniName = "apricitymedia-jni";     FfBase = @("avcodec-62","avformat-62","avutil-60","swresample-6","swscale-9");     FfInBin = $true;  RuntimeBase = @("apwinpthread_01","libdav1d-*") }
    [pscustomobject]@{ Name = "macos-arm64";  Ext = "dylib"; JniName = "libapricitymedia-jni";  FfBase = @("libavcodec.62","libavformat.62","libavutil.60","libswresample.6","libswscale.9"); FfInBin = $false }
    [pscustomobject]@{ Name = "linux-x64";    Ext = "so";    JniName = "libapricitymedia-jni";  FfBase = @("libavcodec.so.62","libavformat.so.62","libavutil.so.60","libswresample.so.6","libswscale.so.9"); FfInBin = $false }
    [pscustomobject]@{ Name = "android-arm64";Ext = "so";    JniName = "libapricitymedia-jni";  FfBase = @("libavcodec.so.62","libavformat.so.62","libavutil.so.60","libswresample.so.6","libswscale.so.9"); FfInBin = $false }
)

# ---- Java -> Minecraft version mapping ----
$mcVersions = @(
    [pscustomobject]@{ JavaLabel = "java17"; McVersion = "1.20.1"; Runtime = "jni"    }
    [pscustomobject]@{ JavaLabel = "java21"; McVersion = "1.21.1"; Runtime = "jni"    }
    [pscustomobject]@{ JavaLabel = "java25"; McVersion = "26.1";   Runtime = "ffmapi" }
)

$null = New-Item -ItemType Directory -Path $OutputDir -Force

function New-TempDir([string]$prefix) {
    $base = [System.IO.Path]::GetTempPath()
    $dir = Join-Path $base "$prefix-$(Get-Random)"
    $null = New-Item -ItemType Directory -Path $dir -Force
    return $dir
}

foreach ($p in $platformDefs) {
    if ($OnlyPlatforms -and $OnlyPlatforms.Count -gt 0 -and ($OnlyPlatforms -notcontains $p.Name)) {
        continue
    }

    $zipFile = Join-Path $ArtifactsDir "ffmpeg-jni-$($p.Name).zip"
    if (-not (Test-Path $zipFile)) {
        Write-Warning "Skipping $($p.Name) - not found at $zipFile"
        continue
    }

    Write-Host "=== $($p.Name) ===" -ForegroundColor Cyan

    $tmp = New-TempDir "ffmpeg-pkg"
    try {
        Expand-Archive -Path $zipFile -DestinationPath $tmp -Force

        # Locate bin/ and lib/  (depth varies - use recursive search)
        $binDir = Get-ChildItem -Recurse -Directory $tmp | Where-Object Name -eq 'bin' | Select-Object -First 1
        $libDir = Get-ChildItem -Recurse -Directory $tmp | Where-Object Name -eq 'lib' | Select-Object -First 1

        if (-not $binDir) { throw "No bin/ in $zipFile" }

        foreach ($jv in $mcVersions) {
            $stage = Join-Path $OutputDir "_stage-$($p.Name)-$($jv.JavaLabel)"
            $null = New-Item -ItemType Directory -Path $stage -Force

            try {
                $ffDir = if ($p.FfInBin) { $binDir.FullName } else { $libDir.FullName }
                if (-not $ffDir -or -not (Test-Path $ffDir)) {
                    Write-Warning "  $($jv.JavaLabel): FFmpeg lib source dir not found"
                    continue
                }

                # ---- Runtime-specific payload ----
                if ($jv.Runtime -eq 'jni') {
                    # JNI library (rename: strip the javaNN suffix)
                    $jniSrc = Join-Path $binDir.FullName "$($p.JniName)-$($jv.JavaLabel).$($p.Ext)"
                    if (-not (Test-Path $jniSrc)) {
                        Write-Warning "  $($jv.JavaLabel): JNI not found, skipping"
                        continue
                    }
                    Copy-Item -Path $jniSrc -Destination (Join-Path $stage "$($p.JniName).$($p.Ext)")
                }
                elseif ($jv.Runtime -eq 'ffmapi') {
                    # Java 25 / MC 26.1 uses FFM API — ship the JNI-free library.
                    # Built by: build-minimal.ps1 build-ffmapi
                    $ffmapiSrc = Join-Path $binDir.FullName "am_ffmpeg.$($p.Ext)"
                    if (-not (Test-Path $ffmapiSrc)) {
                        # Might be in lib/ instead of bin/
                        $ffmapiSrc = Join-Path $ffDir "am_ffmpeg.$($p.Ext)"
                    }
                    if (Test-Path $ffmapiSrc) {
                        Copy-Item -Path $ffmapiSrc -Destination (Join-Path $stage "am_ffmpeg.$($p.Ext)")
                    } else {
                        Write-Warning "  $($jv.JavaLabel): am_ffmpeg.$($p.Ext) not found"
                    }
                }
                else {
                    Write-Warning "  $($jv.JavaLabel): Unknown Runtime='$($jv.Runtime)', skipping"
                    continue
                }

                # ---- FFmpeg shared libraries ----
                foreach ($base in $p.FfBase) {
                    $src = Join-Path $ffDir "$base.$($p.Ext)"
                    if (Test-Path $src) {
                        Copy-Item -Path $src -Destination $stage
                    } else {
                        Write-Warning "  $($jv.JavaLabel): $base.$($p.Ext) not found"
                    }
                }

                # ---- Windows MinGW runtime DLLs ----
                if ($p.Name -eq 'windows-x64' -and $p.PSObject.Properties.Name -contains 'RuntimeBase') {
                    foreach ($base in $p.RuntimeBase) {
                        $pattern = "$base.$($p.Ext)"
                        $hits = Get-ChildItem -Path $binDir.FullName -Filter $pattern -File -ErrorAction SilentlyContinue
                        if ($hits -and $hits.Count -gt 0) {
                            foreach ($h in $hits) {
                                Copy-Item -Path $h.FullName -Destination $stage -Force
                            }
                        } else {
                            Write-Warning "  $($jv.JavaLabel): runtime pattern $pattern not found"
                        }
                    }
                }

                # ---- Package ----
                $prefix = if ($jv.Runtime -eq 'ffmapi') { 'ffmpeg-ffmapi' } else { 'ffmpeg-jni' }
                $outZip = Join-Path $OutputDir "$prefix-mc-$($jv.McVersion)-$($p.Name).zip"
                $stageGlob = Join-Path $stage '*'
                Compress-Archive -Path $stageGlob -DestinationPath $outZip -CompressionLevel Optimal -Force
                Write-Host "  MC $($jv.McVersion) -> $outZip" -ForegroundColor Green
            }
            finally {
                Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
            }
        }
    }
    finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

Write-Host "`nDone. Output in: $OutputDir" -ForegroundColor Cyan

