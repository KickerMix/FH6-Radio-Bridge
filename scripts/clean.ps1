$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

Get-ChildItem -Path $Root -Directory -Recurse -Include bin,obj |
    Where-Object { $_.FullName -like "$Root*" } |
    Remove-Item -Recurse -Force

$HookBuild = Join-Path $Root "hook\build"
if (Test-Path $HookBuild) {
    Remove-Item -LiteralPath $HookBuild -Recurse -Force
}
