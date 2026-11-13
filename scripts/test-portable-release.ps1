#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Archive)

$ErrorActionPreference = 'Stop'
$work = Join-Path ([IO.Path]::GetTempPath()) ('Dictophone portable ' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $work 'Unpacked release'
Expand-Archive -LiteralPath $Archive -DestinationPath $package
$app = Join-Path $package 'Dictophone.exe'
$ffmpeg = Join-Path $package 'ffmpeg.exe'
foreach ($file in $app, $ffmpeg) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing from ZIP: $file" }
}

function Assert-PortableModules([Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.HasExited) { throw "Process exited during startup with code $($Process.ExitCode)." }
    $modules = @($Process.Modules)
    if (!$modules.Count) { throw "Could not inspect DLLs for $($Process.ProcessName)." }
    foreach ($module in $modules) {
        if ($module.FileName.StartsWith("$package\", [StringComparison]::OrdinalIgnoreCase)) { continue }
        $isWindowsModule = $module.FileName.StartsWith("$env:SystemRoot\", [StringComparison]::OrdinalIgnoreCase)
        $isRedistributable = $module.ModuleName -match '^(vcruntime|msvcp\d|msvcr\d|concrt\d|vcomp\d|mfc\d|libgcc|libstdc\+\+|libwinpthread)'
        if (!$isWindowsModule -or $isRedistributable) {
            throw "$($Process.ProcessName) loaded a DLL from outside Windows or the ZIP: $($module.FileName)"
        }
    }
    Write-Host "PASS: $($Process.ProcessName) loaded only Windows DLLs and files from the ZIP."
}

$originalPath = $env:Path
$application = $null
$encoder = $null
Push-Location $work
try {
    # The hosted image includes developer tools. Keep them out of executable and DLL searches.
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    $appInfo = [Diagnostics.ProcessStartInfo]::new($app)
    $appInfo.UseShellExecute = $false
    $appInfo.WorkingDirectory = $work
    $appInfo.ArgumentList.Add('--data-dir')
    $appInfo.ArgumentList.Add((Join-Path $work 'Settings'))
    $application = [Diagnostics.Process]::Start($appInfo)
    $startupDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $application.Refresh()
        if ($application.HasExited) { throw "Dictophone exited before opening its window. Code: $($application.ExitCode)." }
        if ($application.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $startupDeadline)
    if ($application.MainWindowTitle -ne 'Dictophone') { throw 'Dictophone did not open its main window.' }
    Assert-PortableModules $application
    if (!$application.CloseMainWindow() -or !$application.WaitForExit(5000) -or $application.ExitCode -ne 0) {
        throw 'Dictophone did not close normally.'
    }
    Write-Host 'PASS: Dictophone opened its main window and closed normally.'

    # Use synthetic audio because the hosted runners do not supply a test microphone.
    # Keep stdin open briefly to check what the bundled encoder loads at runtime.
    $wave = Join-Path $work 'Test audio.wav'
    $encoderInfo = [Diagnostics.ProcessStartInfo]::new($ffmpeg)
    $encoderInfo.UseShellExecute = $false
    $encoderInfo.CreateNoWindow = $true
    $encoderInfo.RedirectStandardInput = $true
    $encoderInfo.WorkingDirectory = $work
    foreach ($argument in '-v','error','-xerror','-f','s16le','-ar','48000','-ac','1','-i','pipe:0','-c:a','pcm_s16le',$wave) {
        $encoderInfo.ArgumentList.Add($argument)
    }
    $encoder = [Diagnostics.Process]::Start($encoderInfo)
    Start-Sleep -Milliseconds 500
    Assert-PortableModules $encoder
    $pcm = [byte[]]::new(24000)
    $encoder.StandardInput.BaseStream.Write($pcm, 0, $pcm.Length)
    $encoder.StandardInput.Close()
    if (!$encoder.WaitForExit(10000) -or $encoder.ExitCode -ne 0) { throw 'Bundled FFmpeg failed to create WAV.' }

    $formats = @(
        @{ Extension = 'wav';  Decoder = 'wav' }
        @{ Extension = 'mp3';  Decoder = 'mp3';  Codec = 'libmp3lame' }
        @{ Extension = 'ogg';  Decoder = 'ogg';  Codec = 'libvorbis' }
        @{ Extension = 'flac'; Decoder = 'flac'; Codec = 'flac' }
        @{ Extension = 'm4a';  Decoder = 'mov';  Codec = 'aac' }
    )
    foreach ($format in $formats) {
        $file = $wave
        if ($format.Codec) {
            $file = Join-Path $work "Test audio.$($format.Extension)"
            & $ffmpeg -v error -xerror -nostdin -i $wave -c:a $format.Codec $file
            if ($LASTEXITCODE -ne 0) { throw "Encoding $($format.Extension) failed." }
        }
        if ((Get-Item -LiteralPath $file).Length -le 100) { throw "Audio file is empty: $file" }
        & $ffmpeg -v error -xerror -nostdin -f $format.Decoder -i $file -map 0:a:0 -f null -
        if ($LASTEXITCODE -ne 0) { throw "Decoding $($format.Extension) failed." }
        Write-Host "PASS: $($format.Extension.ToUpperInvariant()) encoded and decoded using only the bundled FFmpeg."
    }
} finally {
    foreach ($process in $application, $encoder) {
        if ($process) {
            if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
    Pop-Location
    $env:Path = $originalPath
}
