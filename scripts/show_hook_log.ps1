param(
    [string]$GameDir,

    [int]$Tail = 120
)

$ErrorActionPreference = 'Stop'

$candidates = @()

if ($GameDir) {
    $resolvedGameDir = Resolve-Path -LiteralPath $GameDir
    $candidates += Join-Path $resolvedGameDir.Path 'fh6-radio-bridge\logs\hook.log'
}

if ($env:LOCALAPPDATA) {
    $candidates += Join-Path $env:LOCALAPPDATA 'FH6RadioBridge\hook.log'
}

$logPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $logPath) {
    Write-Host 'No hook log found. Checked:'
    foreach ($candidate in $candidates) {
        Write-Host "  $candidate"
    }
    exit 1
}

Get-Content -LiteralPath $logPath -Tail $Tail
