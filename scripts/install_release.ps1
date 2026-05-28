param(
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

$releaseRoot = Split-Path -Parent $PSScriptRoot
$sourceDll = Join-Path $releaseRoot 'GameFiles\version.dll'

if (-not (Test-Path -LiteralPath $sourceDll)) {
    throw "Release payload is incomplete. Missing: $sourceDll"
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

$running = Test-ProcessFromDirectory $gameRoot
if ($running.Count -gt 0) {
    $names = ($running | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '
    throw "Refusing to install while processes are running from the game directory: $names"
}

$audioDir = Join-Path $gameRoot 'media\audio'
if (-not (Test-Path -LiteralPath $audioDir)) {
    throw "This does not look like the FH6 Content folder. Audio directory was not found: $audioDir"
}

$radioInfoFiles = @(Get-ChildItem -LiteralPath $audioDir -File -Filter 'RadioInfo_*.xml' -ErrorAction SilentlyContinue)
if ($radioInfoFiles.Count -eq 0) {
    throw "No RadioInfo_*.xml files found in $audioDir"
}

$destination = Join-Path $gameRoot 'version.dll'
$installRoot = Join-Path $gameRoot 'fh6-radio-bridge'
$manifestPath = Join-Path $installRoot 'install_manifest.json'
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceDll).Hash
$backupPath = $null
$existingManifest = $null

if (Test-Path -LiteralPath $manifestPath) {
    try {
        $existingManifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        $backupPath = $existingManifest.backupPath
    } catch {
        if (-not $Force) {
            throw "Existing install manifest is unreadable: $manifestPath. Re-run with -Force only if you are sure."
        }
    }
}

if (Test-Path -LiteralPath $destination) {
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($destinationHash -eq $sourceHash) {
        Write-Host "Proxy DLL is already installed: $destination"
    } else {
        $isExistingBridgeInstall = $existingManifest -and $existingManifest.versionDllSha256 -eq $destinationHash
        if ($isExistingBridgeInstall) {
            Write-Host 'Updating existing FH6 Radio Bridge proxy DLL.'
        } elseif (-not $Force) {
            throw "A different version.dll already exists at $destination. Re-run Install.bat with -Force to back it up and replace it."
        } else {
            $timestamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss')
            $backupPath = Join-Path $gameRoot "version.dll.fh6rb-backup-$timestamp"
            Copy-Item -LiteralPath $destination -Destination $backupPath -Force
            Write-Host "Backed up existing version.dll to $backupPath"
        }
    }
}

Write-Host 'Patching radio metadata XML...'
$installArgs = @(
    '-GameDir', $gameRoot,
    '-UseReferenceAnchor',
    '-DisplayName', 'FH6 Radio Bridge',
    '-Artist', 'External Audio'
)
if ($Force) {
    $installArgs += '-Force'
}
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'install_radio_replacement.ps1') -Arguments $installArgs

Write-Host 'Installing proxy DLL...'
New-Item -ItemType Directory -Force -Path $installRoot | Out-Null
Copy-Item -LiteralPath $sourceDll -Destination $destination -Force

$manifest = [ordered]@{
    tool = 'FH6 Radio Bridge release installer'
    installedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    gameDir = $gameRoot
    sourcePath = $sourceDll
    destinationPath = $destination
    versionDllSha256 = $sourceHash
    backupPath = $backupPath
    scope = 'version.dll proxy, radio XML replacement, audio gate, FMOD injector flag, optional game events'
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host 'Activating radio audio gate...'
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'activate_radio_audio.ps1') -Arguments @('-GameDir', $gameRoot)

Write-Host 'Enabling game event monitor...'
Invoke-CheckedScript -ScriptPath (Join-Path $PSScriptRoot 'enable_game_events.ps1') -Arguments @('-GameDir', $gameRoot)

Set-Content -LiteralPath (Join-Path $installRoot 'enable_fmod_inject.flag') -Value 'enabled' -Encoding ASCII
Write-Host "Enabled FMOD injector flag: $(Join-Path $installRoot 'enable_fmod_inject.flag')"

Write-Host ''
Write-Host 'Install complete.'
Write-Host "Game folder: $gameRoot"
Write-Host 'Next steps:'
Write-Host '  1) Run BridgeRadio.bat and choose your CABLE/Hi-Fi Cable Output capture device.'
Write-Host '  2) Open http://localhost:8420/ for the dashboard.'
Write-Host '  3) Launch FH6 and select the replaced radio station.'
