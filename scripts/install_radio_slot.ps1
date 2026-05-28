param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

    [string]$DisplayName = 'FH6 Radio Bridge',

    [string]$Artist = 'External Audio',

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

function Save-XmlDocument([xml]$Document, [string]$Path) {
    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.Indent = $false
    $settings.NewLineHandling = [System.Xml.NewLineHandling]::None

    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try {
        $Document.Save($writer)
    } finally {
        $writer.Dispose()
    }
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

$running = Test-ProcessFromDirectory $gameRoot
if ($running.Count -gt 0) {
    $names = ($running | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '
    throw "Refusing to modify radio XML while processes are running from the game directory: $names"
}

$audioDir = Join-Path $gameRoot 'media\audio'
if (-not (Test-Path -LiteralPath $audioDir)) {
    throw "Audio directory not found: $audioDir"
}

$radioInfoFiles = @(Get-ChildItem -LiteralPath $audioDir -File -Filter 'RadioInfo_*.xml')
if ($radioInfoFiles.Count -eq 0) {
    throw "No RadioInfo_*.xml files found in $audioDir"
}

$installRoot = Join-Path $gameRoot 'fh6-radio-bridge'
$backupRoot = Join-Path $installRoot 'backups'
$manifestPath = Join-Path $installRoot 'radio_slot_manifest.json'

if ((Test-Path -LiteralPath $manifestPath) -and -not $Force) {
    throw "Radio slot manifest already exists at $manifestPath. Run uninstall_radio_slot.ps1 first, or use -Force to refresh metadata."
}

New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
$timestamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss')
$entries = @()

foreach ($file in $radioInfoFiles) {
    $backupPath = Join-Path $backupRoot "$($file.Name).fh6rb-backup-$timestamp"
    Copy-Item -LiteralPath $file.FullName -Destination $backupPath

    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.Load($file.FullName)

    $station = $document.SelectSingleNode('/Radio/RadioStations/RadioStation[@Number="10"]')
    if (-not $station) {
        throw "Station 10 was not found in $($file.FullName)"
    }

    $trackSamples = $station.SelectNodes('SampleList[@Type="Track"]/Sample')
    if (-not $trackSamples -or $trackSamples.Count -eq 0) {
        throw "Station 10 has no track samples in $($file.FullName)"
    }

    foreach ($sample in $trackSamples) {
        $sample.SetAttribute('DisplayName', $DisplayName)
        $sample.SetAttribute('Artist', $Artist)
        $sample.SetAttribute('IsXCloudModeSafe', 'true')
    }

    Save-XmlDocument -Document $document -Path $file.FullName

    $entries += [ordered]@{
        path = $file.FullName
        backupPath = $backupPath
        originalSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $backupPath).Hash
        modifiedSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        stationNumber = 10
        displayName = $DisplayName
        artist = $Artist
        samplesUpdated = $trackSamples.Count
    }
}

$manifest = [ordered]@{
    tool = 'FH6 Radio Bridge install_radio_slot.ps1'
    installedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    gameDir = $gameRoot
    scope = 'local RadioInfo_*.xml Station 10 metadata only; no FMOD banks, textures, binaries, or third-party mod files installed'
    entries = $entries
}

$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Installed FH6 Radio Bridge metadata for Station 10 in $($entries.Count) RadioInfo file(s)."
Write-Host "Manifest: $manifestPath"
Write-Host 'Set Streamer Mode = On and Radio DJ = Off in-game. Use F8 to toggle the FH6 Radio Bridge audio gate.'
