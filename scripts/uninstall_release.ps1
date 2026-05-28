param(
    [string]$GameDir,
    [switch]$Force,
    [switch]$KeepBackup
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

function Invoke-CheckedScript([string]$ScriptPath, [string[]]$Arguments) {
    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Required script is missing: $ScriptPath"
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Script failed with exit code ${LASTEXITCODE}: $ScriptPath"
    }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = Read-Host 'Enter FH6 Content folder path'
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    throw 'GameDir is empty.'
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

$running = Test-ProcessFromDirectory $gameRoot
if ($running.Count -gt 0) {
    $names = ($running | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '
    throw "Refusing to uninstall while processes are running from the game directory: $names"
}

Write-Host 'Deactivating radio audio gate...'
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'deactivate_radio_audio.ps1') -Arguments @('-GameDir', $gameRoot)

Write-Host 'Disabling game event monitor...'
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'disable_game_events.ps1') -Arguments @('-GameDir', $gameRoot)

Write-Host 'Restoring radio metadata XML...'
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'uninstall_radio_replacement.ps1') -Arguments @('-GameDir', $gameRoot)

$installRoot = Join-Path $gameRoot 'fh6-radio-bridge'
$manifestPath = Join-Path $installRoot 'install_manifest.json'
$destination = Join-Path $gameRoot 'version.dll'
$manifest = $null

if (Test-Path -LiteralPath $manifestPath) {
    try {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    } catch {
        if (-not $Force) {
            throw "Install manifest is unreadable: $manifestPath. Re-run Uninstall.bat with -Force only if you are sure."
        }
    }
}

if (Test-Path -LiteralPath (Join-Path $installRoot 'enable_fmod_inject.flag')) {
    Remove-Item -LiteralPath (Join-Path $installRoot 'enable_fmod_inject.flag') -Force
    Write-Host 'Removed FMOD injector flag.'
}

if (-not (Test-Path -LiteralPath $destination)) {
    Write-Host "No version.dll found at $destination"
} else {
    $removeProxy = $true
    if ($manifest -and $manifest.versionDllSha256) {
        $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        if ($destinationHash -ne $manifest.versionDllSha256 -and -not $Force) {
            throw "version.dll does not match the FH6 Radio Bridge manifest hash. Re-run with -Force only if you are sure this file should be removed."
        }
    } elseif (-not $Force) {
        $removeProxy = $false
        Write-Warning "No FH6 Radio Bridge install manifest found. Not touching $destination. Re-run with -Force only if you are sure."
    }

    if ($removeProxy) {
        Remove-Item -LiteralPath $destination -Force
        Write-Host "Removed proxy DLL: $destination"
    }
}

if ($manifest -and $manifest.backupPath -and -not $KeepBackup) {
    if (Test-Path -LiteralPath $manifest.backupPath) {
        Move-Item -LiteralPath $manifest.backupPath -Destination $destination -Force
        Write-Host "Restored previous version.dll backup to $destination"
    } else {
        Write-Warning "Recorded version.dll backup does not exist: $($manifest.backupPath)"
    }
}

if (Test-Path -LiteralPath $manifestPath) {
    Remove-Item -LiteralPath $manifestPath -Force
    Write-Host "Removed install manifest: $manifestPath"
}

Write-Host ''
Write-Host 'Uninstall complete.'
Write-Host "Game folder: $gameRoot"
Write-Host 'Backups and logs inside fh6-radio-bridge were left in place where applicable.'
