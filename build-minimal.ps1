#requires -Version 7.0

# ================================================================
# build-minimal.ps1 — Build minimal FFmpeg 8.1 + JNI wrapper DLL
#
# Prerequisites (MSYS2 MINGW64):
#   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make `
#             mingw-w64-x86_64-pkg-config diffutils
#
# Usage:
#   .\build-minimal.ps1 configure          # Step 1
#   .\build-minimal.ps1 build-ffmpeg       # Step 2
#   $env:JAVA_HOME = "C:\Program Files\Java\jdk-21"
#   .\build-minimal.ps1 build-jni          # Step 3
#   .\build-minimal.ps1 all                # All in one shot
#
# Note: configure and build-ffmpeg run inside MSYS2. The script auto-detects
#       MSYS2 from common install paths. Override with $env:MSYS2_ROOT.
# ================================================================

$ErrorActionPreference = 'Stop'

$ScriptRoot = $PSScriptRoot
$JniDir     = Join-Path $ScriptRoot "jni"
$BuildDir   = Join-Path $ScriptRoot "build-minimal"
$FfmpegSrc  = $ScriptRoot

# ---- configure flags ----
$ConfigureFlags = @(
    '--arch=x86_64'
    '--enable-shared'
    '--disable-static'
    '--disable-programs'
    '--disable-doc'
    '--disable-everything'
    '--disable-avdevice'
    '--disable-avfilter'
    '--disable-swscale-alpha'
    '--disable-runtime-cpudetect'
    '--disable-debug'
    '--enable-small'

    # TLS for HTTPS on Windows (uses SChannel, no external libs)
    '--enable-schannel'

    # --- Video decoders ---
    '--enable-decoder=h264'
    '--enable-decoder=hevc'
    '--enable-decoder=vp8'
    '--enable-decoder=vp9'
    '--enable-decoder=av1'
    '--enable-decoder=mpeg4'
    '--enable-decoder=mpeg2video'

    # --- Audio decoders ---
    '--enable-decoder=aac'
    '--enable-decoder=aac_fixed'
    '--enable-decoder=mp3float'
    '--enable-decoder=vorbis'
    '--enable-decoder=opus'
    '--enable-decoder=flac'
    '--enable-decoder=alac'
    '--enable-decoder=pcm_s16le'
    '--enable-decoder=pcm_s24le'
    '--enable-decoder=pcm_f32le'
    '--enable-decoder=pcm_s32le'
    '--enable-decoder=ac3'
    '--enable-decoder=ac3_fixed'
    '--enable-decoder=eac3'
    '--enable-decoder=wmav2'

    # --- Demuxers ---
    '--enable-demuxer=mov'
    '--enable-demuxer=matroska'
    '--enable-demuxer=mp3'
    '--enable-demuxer=ogg'
    '--enable-demuxer=flac'
    '--enable-demuxer=wav'
    '--enable-demuxer=aac'
    '--enable-demuxer=ac3'
    '--enable-demuxer=eac3'
    '--enable-demuxer=mpegts'
    '--enable-demuxer=hls'
    '--enable-demuxer=flv'
    '--enable-demuxer=aiff'
    '--enable-demuxer=asf'

    # --- Protocols ---
    '--enable-protocol=file'
    '--enable-protocol=http'
    '--enable-protocol=https'
    '--enable-protocol=tcp'

    # --- Bitstream filters (needed for TS/HLS demuxing) ---
    '--enable-bsf=h264_mp4toannexb'
    '--enable-bsf=hevc_mp4toannexb'

    # --- Parsers ---
    '--enable-parser=h264'
    '--enable-parser=hevc'
    '--enable-parser=vp8'
    '--enable-parser=vp9'
    '--enable-parser=aac'
    '--enable-parser=opus'
    '--enable-parser=vorbis'
    '--enable-parser=flac'
    '--enable-parser=mpegaudio'
)

# ================================================================
# Helpers
# ================================================================

function Write-Step($msg) {
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Convert-ToMsysPath($winPath) {
    # "C:\foo\bar" → "/c/foo/bar"
    if (-not $winPath) { return $winPath }
    $result = $winPath -replace '\\', '/'
    if ($result -match '^([a-zA-Z]):(.*)') {
        $result = "/$($Matches[1].ToLower())$($Matches[2])"
    }
    return $result
}

function Find-Msys2Bash {
    # Check $env:MSYS2_ROOT first, then common install paths
    $candidates = @()
    if ($env:MSYS2_ROOT) {
        $candidates += Join-Path $env:MSYS2_ROOT "usr/bin/bash.exe"
    }
    $candidates += @(
        "C:\msys64\usr\bin\bash.exe",
        "C:\msys32\usr\bin\bash.exe"
    )

    foreach ($p in $candidates) {
        if (Test-Path $p) { return (Get-Item $p).FullName }
    }

    # Last resort: anything on PATH (warn the user)
    $onPath = Get-Command 'bash.exe' -ErrorAction SilentlyContinue
    if ($onPath) {
        Write-Warning "MSYS2 not found at common paths, using bash.exe from PATH: $($onPath.Source)"
        Write-Warning "Set `$env:MSYS2_ROOT if this is wrong, e.g.: `$env:MSYS2_ROOT = 'C:\msys64'"
        return $onPath.Source
    }

    throw @"
MSYS2 bash.exe not found. Install MSYS2 from https://www.msys2.org/
Then install build tools:
  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-pkg-config diffutils
"@
}

# ----------------------------------------------------------------
# Run a shell script inside MSYS2.
# Writes $body to a temp file and executes it via MSYS2 bash --login.
# Returns the temp file path so callers can read ffbuild/config.log etc.
# ----------------------------------------------------------------
function Invoke-Msys2Script($body) {
    $bashPath = Find-Msys2Bash

    # Use --login so MSYS2 profile loads mingw64/bin into PATH.
    # Must set MSYSTEM=MINGW64 so /etc/profile sources the MINGW64
    # profile fragment, which adds /mingw64/bin to PATH (where gcc is).
    $fullCmd = $body -replace "`r`n", " " -replace "`n", " " -replace "`r", " "
    $fullCmd = $fullCmd.Trim()

    Write-Host "  $fullCmd" -ForegroundColor Gray

    # Explicitly set MSYSTEM so MINGW64 profile loads
    $prevMsys = $env:MSYSTEM
    $env:MSYSTEM = 'MINGW64'

    & $bashPath --login -c $fullCmd

    $env:MSYSTEM = $prevMsys
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 script failed (exit $LASTEXITCODE). Check output above."
    }
}

# ================================================================
# Commands
# ================================================================

function Invoke-Configure {
    Write-Step "Configuring minimal FFmpeg"

    $null = New-Item -ItemType Directory -Path $BuildDir -Force

    $srcdir  = Convert-ToMsysPath $FfmpegSrc
    $build   = Convert-ToMsysPath $BuildDir
    $prefix  = Convert-ToMsysPath (Join-Path $BuildDir "dist")
    $libdir  = Convert-ToMsysPath (Join-Path $BuildDir "dist/lib")
    $incdir  = Convert-ToMsysPath (Join-Path $BuildDir "dist/include")
    $bindir  = Convert-ToMsysPath (Join-Path $BuildDir "dist/bin")
    $flags   = $ConfigureFlags -join ' '

    # Single-line to avoid continuation & quoting issues in bash -c
    $script = "cd '$build' && '$srcdir/configure' $flags --prefix='$prefix' --libdir='$libdir' --incdir='$incdir' --bindir='$bindir'"

    Invoke-Msys2Script $script

    # Summary
    $log = Join-Path $BuildDir "ffbuild/config.log"
    if (Test-Path $log) {
        Write-Host "`n--- Configuration summary ---" -ForegroundColor Yellow
        Select-String -Path $log -Pattern '^(Enabled|Disabled|External libraries|Protocols|Decoders|Demuxers|Parsers|BSFs)' | ForEach-Object {
            Write-Host $_.Line -ForegroundColor Gray
        }
    }
    Write-Host "`nConfigure done. Next: .\build-minimal.ps1 build-ffmpeg" -ForegroundColor Green
}

function Invoke-BuildFfmpeg {
    Write-Step "Building minimal FFmpeg"

    if (-not (Test-Path (Join-Path $BuildDir "Makefile"))) {
        throw "Makefile not found in $BuildDir. Run '.\build-minimal.ps1 configure' first."
    }

    $jobs = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).NumberOfLogicalProcessors
    if (-not $jobs -or $jobs -le 0) { $jobs = 4 }

    $build = Convert-ToMsysPath $BuildDir
    $script = "cd '$build' && make -j$jobs && make install"
    Invoke-Msys2Script $script

    $libDir = Join-Path $BuildDir "dist/lib"
    Write-Host "`nBuild complete. Libraries in: $libDir" -ForegroundColor Green
    Get-ChildItem "$libDir/*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  $($_.Name) ($('{0:N0} KB' -f ($_.Length / 1KB)))"
    }
}

function Invoke-BuildJni {
    $javahome = $env:JAVA_HOME
    if (-not $javahome) { throw 'JAVA_HOME environment variable is not set.' }

    $ffinc = Join-Path $BuildDir "dist/include"
    $fflib = Join-Path $BuildDir "dist/lib"
    if (-not (Test-Path "$ffinc/libavcodec")) {
        throw "FFmpeg headers not found at $ffinc. Run '.\build-minimal.ps1 build-ffmpeg' first."
    }

    Write-Step "Building JNI wrapper DLL"
    Write-Host "  FFmpeg inc: $ffinc"
    Write-Host "  FFmpeg lib: $fflib"
    Write-Host "  Java home:  $javahome"

    $null = New-Item -ItemType Directory -Path (Join-Path $BuildDir "dist/bin") -Force

    $ffincMsys = Convert-ToMsysPath $ffinc
    $fflibMsys = Convert-ToMsysPath $fflib
    $javaincMsys = Convert-ToMsysPath (Join-Path $javahome "include")
    $jniMd = 'win32'
    $outMsys = Convert-ToMsysPath (Join-Path $BuildDir "dist/bin/apricitymedia-jni.dll")
    $srcMsys = Convert-ToMsysPath (Join-Path $JniDir "jni_ffmpeg.c")

    Write-Host "  Output:   $(Join-Path $BuildDir 'dist/bin/apricitymedia-jni.dll')" -ForegroundColor Gray

    # Build inside MSYS2 so gcc and linker have the MSYS2 runtime
    $script = "gcc -shared -o '$outMsys' -I'$javaincMsys' -I'$javaincMsys/$jniMd' -I'$ffincMsys' -L'$fflibMsys' '$srcMsys' -lavformat -lavcodec -lavutil -lswresample -lswscale -lm -O2 -s -Wl,--enable-runtime-pseudo-reloc -static-libgcc -static-libstdc++"
    Invoke-Msys2Script $script

    $outPath = Join-Path $BuildDir "dist/bin/apricitymedia-jni.dll"
    if (Test-Path $outPath) {
        $item = Get-Item $outPath
        Write-Host "`nJNI DLL built:" -ForegroundColor Green
        Write-Host "  $($item.Name) ($('{0:N0} KB' -f ($item.Length / 1KB)))" -ForegroundColor Green
    } else {
        throw "JNI DLL not produced at $outPath"
    }
}

function Invoke-GenHeader {
    $javahome = $env:JAVA_HOME
    if (-not $javahome) { throw 'JAVA_HOME environment variable is not set.' }

    Write-Step "Generating JNI header from Java class"

    $tmpDir = Join-Path $env:TEMP "apricitymedia-jni-$(Get-Random)"
    $null = New-Item -ItemType Directory -Path $tmpDir -Force
    try {
        $src = Join-Path $JniDir "ApricityMediaNative.java"

        & (Join-Path $javahome "bin/javac") -d $tmpDir $src
        if ($LASTEXITCODE -ne 0) { throw "javac failed" }

        # javac -h works in JDK 8+, javah is deprecated
        & (Join-Path $javahome "bin/javac") -h $JniDir -d $tmpDir $src
        if ($LASTEXITCODE -ne 0) {
            & (Join-Path $javahome "bin/javah") -d $JniDir -classpath $tmpDir cc.sighs.apricitymedia.jni.ApricityMediaNative
        }

        $hdr = Join-Path $JniDir "cc_sighs_apricitymedia_jni_ApricityMediaNative.h"
        if (Test-Path $hdr) {
            Write-Host "Header generated: $hdr" -ForegroundColor Green
        } else {
            Write-Warning "Header not found at expected path: $hdr"
        }
    }
    finally {
        Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
    }
}

function Invoke-All {
    Invoke-Configure
    Invoke-BuildFfmpeg
    Invoke-BuildJni
    Write-Host "`n=== All done ===" -ForegroundColor Cyan
    Write-Host "Output: $(Join-Path $BuildDir 'dist/bin/')" -ForegroundColor Green
}

# ================================================================
# Entry point
# ================================================================

$command = if ($args.Count -gt 0) { $args[0].ToLower() } else { 'all' }

switch ($command) {
    'configure'      { Invoke-Configure }
    'build-ffmpeg'   { Invoke-BuildFfmpeg }
    'build-jni'      { Invoke-BuildJni }
    'gen-header'     { Invoke-GenHeader }
    'all'            { Invoke-All }
    default {
        Write-Host "Usage: $($MyInvocation.MyCommand.Name) [configure|build-ffmpeg|build-jni|gen-header|all]" -ForegroundColor Yellow
        exit 1
    }
}
