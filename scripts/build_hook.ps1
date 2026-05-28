param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

$CMake = Get-Command cmake -ErrorAction SilentlyContinue
if ($CMake) {
    $CMakeExe = $CMake.Source
}
else {
    $VsCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $VsCMake)) {
        throw "cmake.exe was not found in PATH or Visual Studio Community installation."
    }

    $CMakeExe = $VsCMake
}

Push-Location $Root
try {
    & $CMakeExe -S .\hook -B .\hook\build -A x64
    & $CMakeExe --build .\hook\build --config $Configuration
}
finally {
    Pop-Location
}
