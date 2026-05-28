param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

    [string]$SoundName = 'FH6_RadioBridge_Stream',

    [string]$DisplayName = 'FH6 Radio Bridge',

    [string]$Artist = 'External Audio',

    [int]$SampleLength = 13230000,

    [int]$SampleRate = 48000,

    [switch]$UseReferenceAnchor,

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

function New-Entry([xml]$Document, [string]$Name) {
    $entry = $Document.CreateElement('Entry')
    $entry.SetAttribute('Name', $Name)
    return $entry
}

function Add-UniqueName([System.Collections.Generic.List[string]]$Names, [string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        return
    }

    if (-not $Names.Contains($Name)) {
        $Names.Add($Name)
    }
}

function Get-SpotifyStylePlaylistNames([xml]$Document, [string]$AnchorSoundName) {
    $names = [System.Collections.Generic.List[string]]::new()

    $playlistEntries = $Document.SelectNodes('/Radio/RadioStations/RadioStation[@Number!="10"]/PlayList[@Type="FreeRoam" or @Type="Event"]/Entry')
    foreach ($entry in $playlistEntries) {
        Add-UniqueName -Names $names -Name $entry.Name
    }

    if ($names.Count -eq 0) {
        $trackSamples = $Document.SelectNodes('/Radio/RadioStations/RadioStation[@Number!="10"]/SampleList[@Type="Track"]/Sample')
        foreach ($sample in $trackSamples) {
            Add-UniqueName -Names $names -Name $sample.SoundName
        }
    }

    Add-UniqueName -Names $names -Name $AnchorSoundName
    return $names
}

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$gameRoot = Get-FullPath $resolvedGameDir.Path

if ($UseReferenceAnchor) {
    $SoundName = 'HZ6_R9_PeterBroderick_EyesClosedandTraveling'
    $SampleLength = 13230000
    $SampleRate = 44100
}

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
$manifestPath = Join-Path $installRoot 'radio_replacement_manifest.json'

if ((Test-Path -LiteralPath $manifestPath) -and -not $Force) {
    throw "Radio replacement manifest already exists at $manifestPath. Run uninstall_radio_replacement.ps1 first, or use -Force to refresh metadata."
}

New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
$timestamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss')
$entries = @()

foreach ($file in $radioInfoFiles) {
    $backupPath = Join-Path $backupRoot "$($file.Name).fh6rb-replacement-backup-$timestamp"
    Copy-Item -LiteralPath $file.FullName -Destination $backupPath

    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.Load($file.FullName)

    $station = $document.SelectSingleNode('/Radio/RadioStations/RadioStation[@Number="10"]')
    if (-not $station) {
        throw "Station 10 was not found in $($file.FullName)"
    }

    $trackList = $station.SelectSingleNode('SampleList[@Type="Track"]')
    if (-not $trackList) {
        throw "Station 10 track sample list was not found in $($file.FullName)"
    }

    $templateSample = $trackList.SelectSingleNode('Sample')
    if (-not $templateSample) {
        throw "Station 10 has no track sample template in $($file.FullName)"
    }

    $replacementSample = $templateSample.CloneNode($true)
    $replacementSample.SetAttribute('SoundName', $SoundName)
    $replacementSample.SetAttribute('SampleLength', [string]$SampleLength)
    $replacementSample.SetAttribute('SampleRate', [string]$SampleRate)
    $replacementSample.SetAttribute('DisplayName', $DisplayName)
    $replacementSample.SetAttribute('Artist', $Artist)
    $replacementSample.SetAttribute('IsXCloudModeSafe', 'true')

    while ($trackList.HasChildNodes) {
        $null = $trackList.RemoveChild($trackList.FirstChild)
    }
    $null = $trackList.AppendChild($replacementSample)

    $playlists = $station.SelectNodes('PlayList[@Type="FreeRoam" or @Type="Event"]')
    $spotifyStylePlaylistNames = $null
    if ($UseReferenceAnchor) {
        $spotifyStylePlaylistNames = Get-SpotifyStylePlaylistNames -Document $document -AnchorSoundName $SoundName
    }

    foreach ($playlist in $playlists) {
        while ($playlist.HasChildNodes) {
            $null = $playlist.RemoveChild($playlist.FirstChild)
        }

        if ($UseReferenceAnchor -and $spotifyStylePlaylistNames -and $spotifyStylePlaylistNames.Count -gt 0) {
            foreach ($name in $spotifyStylePlaylistNames) {
                $null = $playlist.AppendChild((New-Entry -Document $document -Name $name))
            }
        } else {
            $null = $playlist.AppendChild((New-Entry -Document $document -Name $SoundName))
        }
    }

    Save-XmlDocument -Document $document -Path $file.FullName

    $entries += [ordered]@{
        path = $file.FullName
        backupPath = $backupPath
        originalSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $backupPath).Hash
        modifiedSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        stationNumber = 10
        soundName = $SoundName
        useReferenceAnchor = [bool]$UseReferenceAnchor
        sampleLength = $SampleLength
        sampleRate = $SampleRate
        displayName = $DisplayName
        artist = $Artist
        spotifyStylePlaylistEntries = if ($spotifyStylePlaylistNames) { $spotifyStylePlaylistNames.Count } else { 1 }
        replacedPlaylists = @($playlists | ForEach-Object { $_.Type })
    }
}

$manifest = [ordered]@{
    tool = 'FH6 Radio Bridge install_radio_replacement.ps1'
    installedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    gameDir = $gameRoot
    scope = 'Station 10 RadioInfo replacement only; can use either a synthetic missing SoundName for overlay suppression or the existing reference anchor for FMOD/radio-bus diagnostics; no FMOD banks, textures, binaries, or third-party mod files installed'
    entries = $entries
}

$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Installed Station 10 replacement metadata in $($entries.Count) RadioInfo file(s)."
Write-Host "Manifest: $manifestPath"
if ($UseReferenceAnchor) {
    Write-Host 'Reference anchor mode is active with Spotify-style full station playlists; the FMOD injector must replace the active radio channel.'
} else {
    Write-Host 'If the game rejects the synthetic sound, run uninstall_radio_replacement.ps1 to restore the previous RadioInfo XML.'
}
