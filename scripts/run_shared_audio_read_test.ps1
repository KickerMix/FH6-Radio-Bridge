param(
    [int]$Seconds = 10,
    [int]$FramesPerBlock = 960
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Exe = Join-Path $Root "hook\build\Release\SharedAudioReadTest.exe"

if (-not (Test-Path $Exe)) {
    & (Join-Path $PSScriptRoot "build_hook.ps1") -Configuration Release
}

& $Exe $Seconds $FramesPerBlock
