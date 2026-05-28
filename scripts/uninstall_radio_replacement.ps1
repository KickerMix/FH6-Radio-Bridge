param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-ProcessFromDirectory([string]$Directory) {
    $prefix = $Directory.TrimEnd('\') + '\'
    $matches = @()
    foreach ($process in Get-Process) {
        try {
            if ($process.Path -and (Get-FullPath $process.Path).StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $matches += $process
            }
        } catch {
        }
    }

    return $matches
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

$running = Test-ProcessFromDirectory $gameRoot
if ($running.Count -gt 0) {
    $names = ($running | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '
    throw "Refusing to restore radio XML while processes are running from the game directory: $names"
}

$manifestPath = Join-Path $gameRoot 'fh6-radio-bridge\radio_replacement_manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    Write-Host "No radio replacement manifest found: $manifestPath"
    exit 0
}

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
foreach ($entry in @($manifest.entries)) {
    if (-not (Test-Path -LiteralPath $entry.backupPath)) {
        throw "Backup missing: $($entry.backupPath)"
    }

    Copy-Item -LiteralPath $entry.backupPath -Destination $entry.path -Force
    Write-Host "Restored $($entry.path)"
}

Remove-Item -LiteralPath $manifestPath
Write-Host "Removed manifest: $manifestPath"
