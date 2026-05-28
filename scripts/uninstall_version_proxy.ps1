param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

    [switch]$RestoreBackup,

    [switch]$Force
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
    throw "Refusing to uninstall while processes are running from the game directory: $names"
}

$destination = Join-Path $gameRoot 'version.dll'
$manifestPath = Join-Path $gameRoot 'fh6-radio-bridge\install_manifest.json'
$manifest = $null

if (Test-Path -LiteralPath $manifestPath) {
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
}

if (-not (Test-Path -LiteralPath $destination)) {
    Write-Host "No version.dll found at $destination"
} else {
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    $expectedHash = if ($manifest) { $manifest.versionDllSha256 } else { $null }

    if ($expectedHash -and $destinationHash -ne $expectedHash -and -not $Force) {
        throw "version.dll does not match the manifest hash. Re-run with -Force only if you are sure this file should be removed."
    }

    if (-not $expectedHash -and -not $Force) {
        throw "No FH6 Radio Bridge manifest was found. Re-run with -Force only if you are sure this version.dll should be removed."
    }

    Remove-Item -LiteralPath $destination
    Write-Host "Removed proxy DLL: $destination"
}

if ($RestoreBackup) {
    if (-not $manifest -or -not $manifest.backupPath) {
        Write-Warning 'No backup path is recorded in the manifest.'
    } elseif (-not (Test-Path -LiteralPath $manifest.backupPath)) {
        Write-Warning "Recorded backup does not exist: $($manifest.backupPath)"
    } else {
        Move-Item -LiteralPath $manifest.backupPath -Destination $destination -Force
        Write-Host "Restored backup to $destination"
    }
}

if (Test-Path -LiteralPath $manifestPath) {
    Remove-Item -LiteralPath $manifestPath
    Write-Host "Removed manifest: $manifestPath"
}
