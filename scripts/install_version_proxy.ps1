param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

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

$repoRoot = Split-Path -Parent $PSScriptRoot
$hookDll = Join-Path $repoRoot 'hook\build\Release\version.dll'
$buildScript = Join-Path $PSScriptRoot 'build_hook.ps1'

if (-not (Test-Path -LiteralPath $hookDll)) {
    Write-Host 'version.dll is missing; building hook first.'
    & $buildScript
}

if (-not (Test-Path -LiteralPath $hookDll)) {
    throw "Hook build did not produce $hookDll"
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

$running = Test-ProcessFromDirectory $gameRoot
if ($running.Count -gt 0) {
    $names = ($running | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '
    throw "Refusing to install while processes are running from the game directory: $names"
}

$knownExeNames = @(
    'forzahorizon6.exe',
    'ForzaHorizon6.exe',
    'ForzaHorizon6-Win64-Shipping.exe'
)

$hasKnownExe = $false
foreach ($name in $knownExeNames) {
    if (Test-Path -LiteralPath (Join-Path $gameRoot $name)) {
        $hasKnownExe = $true
        break
    }
}

if (-not $hasKnownExe) {
    Write-Warning 'No known Forza Horizon 6 executable name was found in this directory. Continuing because executable names can vary by build.'
}

$destination = Join-Path $gameRoot 'version.dll'
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $hookDll).Hash
$installRoot = Join-Path $gameRoot 'fh6-radio-bridge'
$manifestPath = Join-Path $installRoot 'install_manifest.json'
$backupPath = $null
$existingManifest = $null

if (Test-Path -LiteralPath $manifestPath) {
    $existingManifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $backupPath = $existingManifest.backupPath
}

if (Test-Path -LiteralPath $destination) {
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($destinationHash -eq $sourceHash) {
        Write-Host "FH6 Radio Bridge version.dll is already installed at $destination"
    } else {
        $isExistingBridgeInstall = $existingManifest -and $existingManifest.versionDllSha256 -eq $destinationHash
        if ($isExistingBridgeInstall) {
            Write-Host 'Updating existing FH6 Radio Bridge proxy DLL.'
        } elseif (-not $Force) {
            throw "A different version.dll already exists at $destination. Re-run with -Force to back it up and replace it."
        } else {
            $timestamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss')
            $backupPath = Join-Path $gameRoot "version.dll.fh6rb-backup-$timestamp"
            Copy-Item -LiteralPath $destination -Destination $backupPath
            Write-Host "Backed up existing version.dll to $backupPath"
        }
    }
}

New-Item -ItemType Directory -Force -Path $installRoot | Out-Null
Copy-Item -LiteralPath $hookDll -Destination $destination -Force

$manifest = [ordered]@{
    tool = 'FH6 Radio Bridge install_version_proxy.ps1'
    installedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    gameDir = $gameRoot
    sourcePath = (Resolve-Path -LiteralPath $hookDll).Path
    destinationPath = $destination
    versionDllSha256 = $sourceHash
    backupPath = $backupPath
    scope = 'version.dll proxy only; no media XML, FMOD banks, textures, or third-party mod files installed'
}

$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Installed proxy DLL: $destination"
Write-Host "Manifest: $manifestPath"
Write-Host 'Launch the bridge before starting the game, then check fh6-radio-bridge\logs\hook.log in the game directory.'
