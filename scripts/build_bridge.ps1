param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

Push-Location $Root
try {
    dotnet restore .\bridge\FH6RadioBridge.sln
    dotnet build .\bridge\FH6RadioBridge.sln -c $Configuration --no-restore
}
finally {
    Pop-Location
}
