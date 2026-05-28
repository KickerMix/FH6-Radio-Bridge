$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Exe = Join-Path $Root "bridge\src\TestReceiver\bin\Release\net8.0\TestReceiver.exe"
$Project = Join-Path $Root "bridge\src\TestReceiver\TestReceiver.csproj"

if (Test-Path $Exe) {
    & $Exe
}
else {
    dotnet run --project $Project -c Release
}
