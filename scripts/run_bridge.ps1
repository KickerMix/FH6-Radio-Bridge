param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BridgeArgs
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Project = Join-Path $Root "bridge\src\FH6RadioBridge\FH6RadioBridge.csproj"
$Exe = Get-ChildItem -Path (Join-Path $Root "bridge\src\FH6RadioBridge\bin\Release") -Filter "FH6RadioBridge.exe" -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($Exe) {
    & $Exe.FullName @BridgeArgs
}
else {
    dotnet run --project $Project -c Release -- @BridgeArgs
}
