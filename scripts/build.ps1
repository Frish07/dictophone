param([switch]$WithFfmpeg, [switch]$Test, [switch]$Microphone)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force build,app | Out-Null
    $compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    if (!$compiler) {
        foreach ($candidate in @('C:\msys64\ucrt64\bin\g++.exe','C:\msys64\mingw64\bin\g++.exe')) {
            if (Test-Path -LiteralPath $candidate) { $compiler = Get-Item -LiteralPath $candidate; break }
        }
    }
    if (!$compiler) { throw 'Install MSYS2 UCRT64 GCC (mingw-w64-ucrt-x86_64-gcc), or use CMake with Visual Studio 2022.' }
    $compilerPath = if ($compiler.Source) { $compiler.Source } else { $compiler.FullName }
    $env:PATH = (Split-Path -Parent $compilerPath) + ';' + $env:PATH
    $flags = @('-std=c++17','-O2','-s','-Wall','-Wextra','-Wpedantic','-DUNICODE','-D_UNICODE','-DNOMINMAX','-D_WIN32_WINNT=0x0A00','-static','-static-libgcc','-static-libstdc++')
    $libraries = @('-lole32','-luuid','-lksuser','-lcomctl32','-lshell32','-ldwmapi','-lgdi32')
    $windres = Join-Path (Split-Path -Parent $compilerPath) 'windres.exe'
    & $windres src/app.rc -O coff -o build/resource.o
    if ($LASTEXITCODE) { throw 'Resource compilation failed.' }
    & $compilerPath @flags -mwindows -municode src/main.cpp src/audio.cpp build/resource.o -o app/Dictophone.exe @libraries
    if ($LASTEXITCODE) { throw 'C++ compilation failed.' }
    if ($WithFfmpeg) { & "$PSScriptRoot/setup-ffmpeg.ps1" }
    if ($Test -or $Microphone) {
        & $compilerPath @flags -municode tests/audio_tests.cpp src/audio.cpp -o build/AudioTests.exe @libraries
        if ($LASTEXITCODE) { throw 'Test compilation failed.' }
        & build/AudioTests.exe
        if ($LASTEXITCODE) { throw 'Audio tests failed.' }
        if ($Microphone) {
            & build/AudioTests.exe --microphone
            if ($LASTEXITCODE) { throw 'Microphone test failed.' }
        }
    }
    Write-Host "Built: $projectRoot\app\Dictophone.exe"
} finally { Pop-Location }
