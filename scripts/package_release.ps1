param(
    [string]$Configuration = 'Release',
    [string]$OutputRoot = 'artifacts'
)

$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$artifactRoot = Join-Path $root $OutputRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stage = Join-Path $artifactRoot "FH6RadioBridge-manual-$stamp"
$zipPath = "$stage.zip"

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

& (Join-Path $PSScriptRoot 'build_hook.ps1') -Configuration $Configuration

$appDir = Join-Path $stage 'Bridge'
$payloadDir = Join-Path $stage 'GameFiles'
$scriptsDir = Join-Path $stage 'Scripts'
New-Item -ItemType Directory -Force -Path $appDir, $payloadDir, $scriptsDir | Out-Null

dotnet publish (Join-Path $root 'bridge\src\FH6RadioBridge\FH6RadioBridge.csproj') `
    -c $Configuration `
    -r win-x64 `
    --self-contained true `
    -p:DebugType=none `
    -p:DebugSymbols=false `
    -o $appDir

Get-ChildItem -LiteralPath $appDir -Filter '*.pdb' -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force

Copy-Item -LiteralPath (Join-Path $root 'hook\build\Release\version.dll') -Destination (Join-Path $payloadDir 'version.dll')

foreach ($name in @(
    'install_radio_replacement.ps1',
    'uninstall_radio_replacement.ps1',
    'activate_radio_audio.ps1',
    'deactivate_radio_audio.ps1',
    'show_hook_log.ps1',
    'enable_game_events.ps1',
    'disable_game_events.ps1',
    'install_release.ps1',
    'uninstall_release.ps1'
)) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $scriptsDir $name)
}

@'
@echo off
setlocal
set "ROOT=%~dp0"
set "BRIDGE_EXE=%ROOT%Bridge\FH6RadioBridge.exe"
set "DASHBOARD_URL=http://127.0.0.1:8420/"

if not exist "%BRIDGE_EXE%" (
    echo Bridge executable not found: %BRIDGE_EXE%
    pause
    exit /b 1
)

pushd "%ROOT%Bridge"
"%BRIDGE_EXE%" --list-devices
echo.
set /p DEVICE_INDEX=Enter capture device index (Hi-Fi/CABLE Output): 
echo.
echo Starting bridge with device index %DEVICE_INDEX% ...
echo Dashboard will open at %DASHBOARD_URL%
start "" /b powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Sleep -Seconds 2; Start-Process '%DASHBOARD_URL%'"
"%BRIDGE_EXE%" --device-index "%DEVICE_INDEX%"
popd
'@ | Set-Content -LiteralPath (Join-Path $stage 'BridgeRadio.bat') -Encoding ASCII

@'
@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\install_release.ps1" %*
set "EXITCODE=%ERRORLEVEL%"
echo.
if not "%EXITCODE%"=="0" (
    echo Install failed with exit code %EXITCODE%.
) else (
    echo Install finished successfully.
)
pause
exit /b %EXITCODE%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Install.bat') -Encoding ASCII

@'
@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\uninstall_release.ps1" %*
set "EXITCODE=%ERRORLEVEL%"
echo.
if not "%EXITCODE%"=="0" (
    echo Uninstall failed with exit code %EXITCODE%.
) else (
    echo Uninstall finished successfully.
)
pause
exit /b %EXITCODE%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Uninstall.bat') -Encoding ASCII

@"
FH6 Radio Bridge - Manual Release

Recommended install:
1) Close Forza Horizon 6.
2) Run .\Install.bat
3) Enter your FH6 Content folder when asked, for example:
   Z:\XBOX\Forza Horizon 6\Content
4) In your audio player/browser set output device to "CABLE Input" or "Hi-Fi Cable Input".
5) Run .\BridgeRadio.bat and choose capture device "CABLE Output" or "Hi-Fi Cable Output".
6) Open http://localhost:8420 for the live dashboard.
7) Launch the game.
8) In game audio settings:
   Streamer Mode = On
   Radio DJ = Off
9) Select the FH6/Streamer station slot.

Recommended uninstall:
1) Close Forza Horizon 6.
2) Run .\Uninstall.bat
3) Enter the same FH6 Content folder.

Command-line examples:
.\Install.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content"
.\Install.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content" -Force
.\Uninstall.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content"
.\Uninstall.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content" -Force

Manual fallback install:
1) Copy .\GameFiles\version.dll to <FH6 Content>\version.dll.
2) Run:
   powershell -ExecutionPolicy Bypass -File .\Scripts\install_radio_replacement.ps1 -GameDir "<FH6 Content>" -UseReferenceAnchor -DisplayName "FH6 Radio Bridge" -Artist "External Audio"
3) Run:
   powershell -ExecutionPolicy Bypass -File .\Scripts\activate_radio_audio.ps1 -GameDir "<FH6 Content>"
4) Run:
   powershell -ExecutionPolicy Bypass -File .\Scripts\enable_game_events.ps1 -GameDir "<FH6 Content>"
5) Create file <FH6 Content>\fh6-radio-bridge\enable_fmod_inject.flag with text: enabled

Hook log path:
<FH6 Content>\fh6-radio-bridge\logs\hook.log
"@ | Set-Content -LiteralPath (Join-Path $stage 'README_MANUAL.txt') -Encoding UTF8

@"
Third-party notices

fh6-universal-radio
URL: https://github.com/g0ldyy/fh6-universal-radio
License: GPL-3.0
Used as open-source reference for FMOD DSP radio-bus integration pattern.
"@ | Set-Content -LiteralPath (Join-Path $stage 'THIRD_PARTY_NOTICES.txt') -Encoding UTF8

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash
Write-Host "Release staged: $stage"
Write-Host "Release archive: $zipPath"
Write-Host "SHA256: $hash"
