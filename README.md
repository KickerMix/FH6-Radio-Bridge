# FH6 Radio Bridge

FH6 Radio Bridge routes audio that is already playing on your Windows PC into the Forza Horizon 6 radio path.

It is source-agnostic: browser audio, a music player, Spotify, YouTube Music, local files, or any other app can be routed through a virtual audio device and captured by the bridge.

## Features

- Captures audio from a selected WASAPI input device or from default playback loopback.
- Converts audio to 48 kHz stereo float32 PCM.
- Sends PCM to the game-side hook through a named shared-memory ring buffer.
- Installs a `version.dll` proxy hook into the FH6 `Content` folder.
- Replaces the selected radio slot metadata with `FH6 Radio Bridge` / `External Audio`.
- Live dashboard at `http://localhost:8420/`.
- Dashboard controls:
  - output volume;
  - previous / play-pause / next / restart current;
  - race start action: `None`, `Next track`, `Restart current`;
  - Next song hook: quickly change station away and back within 1 second to skip the song;
  - loudness normalization;
  - 5-band EQ: 60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz, ±6 dB per band.
- Optional hook-side race/station event monitor.
- Installer and uninstaller generated in every manual release package.
- Fails safe: if a game pattern/signature is not found, the hook logs a warning and stays inert instead of crashing the game.

## How it works

```text
Audio source / browser / player
        -> virtual audio cable or default loopback
        -> FH6RadioBridge.exe
        -> 48 kHz stereo float32 DSP chain
        -> shared memory ring buffer
        -> version.dll game hook
        -> FH6 radio stream / FMOD DSP replacement path
```

The bridge does not download, decrypt, or bypass DRM for any music service. It only captures audio already playing on the user's system.

## Requirements

For using a packaged release:

- Windows 10 or newer.
- Forza Horizon 6 installed locally.
- Optional but recommended: VB-CABLE, Hi-Fi Cable, or another virtual audio cable for routing one app into the bridge.

For building from source:

- .NET 8 SDK.
- Visual Studio 2022 or Build Tools 2022 with the Desktop C++ workload.
- Windows 10/11 SDK.
- CMake.
- PowerShell 5.1 or newer.

## Build

From the repository root:

```powershell
.\scripts\package_release.ps1
```

The generated release appears under:

```text
artifacts\FH6RadioBridge-manual-<timestamp>\
artifacts\FH6RadioBridge-manual-<timestamp>.zip
```

Build parts separately:

```powershell
.\scripts\build_bridge.ps1
.\scripts\build_hook.ps1
```

Clean local build outputs:

```powershell
.\scripts\clean.ps1
```

## Install packaged release

Extract `FH6RadioBridge-manual-<timestamp>.zip` and run:

```text
Install.bat
```

The installer asks for the FH6 `Content` folder, for example:

```text
Z:\XBOX\Forza Horizon 6\Content
```

The installer performs these actions:

- checks that FH6 is not running;
- verifies that the selected folder looks like the FH6 `Content` folder;
- backs up existing files where applicable;
- patches local `RadioInfo_*.xml` radio metadata;
- copies `GameFiles\version.dll` to the game folder;
- creates `fh6-radio-bridge\install_manifest.json`;
- enables radio audio, game events, and the FMOD injector flag.

Command-line install examples:

```powershell
.\Install.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content"
.\Install.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content" -Force
```

Use `-Force` only when you intentionally want to replace an existing `version.dll`; the installer will create a backup.

## Run

Start the bridge:

```text
BridgeRadio.bat
```

It will:

1. list available capture devices;
2. ask for the device index;
3. start `FH6RadioBridge.exe`;
4. open `http://127.0.0.1:8420/` automatically.

Recommended setup with a virtual cable:

1. Set your music app/browser output to `CABLE Input` or `Hi-Fi Cable Input`.
2. In `BridgeRadio.bat`, choose the matching capture side: `CABLE Output` or `Hi-Fi Cable Output`.
3. Keep the bridge running.
4. Launch FH6.
5. Select the replaced FH6/Streamer radio station slot in game.

Bridge can also be run manually:

```powershell
.\Bridge\FH6RadioBridge.exe --list-devices
.\Bridge\FH6RadioBridge.exe --device-index 3
.\Bridge\FH6RadioBridge.exe --loopback-default
```

## Dashboard

Open:

```text
http://localhost:8420/
```

Useful local API endpoints:

```text
GET  /api/state
GET  /api/metadata
GET  /api/config
PUT  /api/config
POST /api/control/previous
POST /api/control/playpause
POST /api/control/next
POST /api/control/restart
POST /api/hook/event
```

The DSP chain is applied producer-side before audio reaches the game:

```text
capture -> resample 48 kHz stereo -> loudness normalization -> 5-band EQ -> output volume -> limiter -> shared memory
```

Loudness normalization is streaming RMS/AGC-style. It does not pre-scan whole tracks because the bridge captures already-playing audio instead of owning the source files.

## Race start action and Next song hook

The hook can send race/station events to the bridge. The bridge decides whether to act based on dashboard settings.

- Race start action:
  - `None` — do nothing;
  - `Next track` — skip when a race begins;
  - `Restart current` — try to restart current media when a race begins.
- Next song hook:
  - quickly change the radio station away and back within 1 second to skip the current song.

If the current game build changes internal patterns/offsets, these automations remain inactive and the hook log explains why.

## Uninstall packaged release

Close FH6 and run:

```text
Uninstall.bat
```

Command-line example:

```powershell
.\Uninstall.bat -GameDir "Z:\XBOX\Forza Horizon 6\Content"
```

The uninstaller disables flags, restores radio XML from backup, removes the installed proxy DLL, and restores a previous `version.dll` backup when one exists.

## Logs and troubleshooting

Hook log:

```text
<FH6 Content>\fh6-radio-bridge\logs\hook.log
```

Open log through helper script:

```powershell
.\Scripts\show_hook_log.ps1 -GameDir "Z:\XBOX\Forza Horizon 6\Content"
```

Common checks:

- If `http://localhost:8420/` does not open, make sure `BridgeRadio.bat` is still running.
- If the dashboard opens but there is no audio, verify that the selected capture device receives signal in Windows sound settings.
- If using VB-CABLE/Hi-Fi Cable, route the source app to the cable input and select the cable output in the bridge.
- If the hook logs `pattern not found` or `unsupported Forza image size`, the current game build is not recognized by that feature.
- If shared memory falls back from `Global\...` to `Local\...`, run the bridge and game in the same privilege mode. Usually both should run normally, not as administrator.

## Repository layout

```text
bridge/src/FH6RadioBridge/   Bridge app, dashboard/API, capture, DSP, shared-memory writer
bridge/src/TestReceiver/     Standalone shared-memory receiver for testing
hook/src/                    version.dll proxy, shared-memory reader, FMOD/radio hook, game events
scripts/                     build, package, install, uninstall, diagnostics
docs/                        architecture, IPC, legal/safety, development notes
```

## Safety and legal notes

- This project is not affiliated with Microsoft, Playground Games, Turn 10, Forza Horizon, Spotify, YouTube, Yandex, or any other music service.
- This project does not include game files or third-party mod binaries.
- This project modifies local game files when installed; keep backups and use at your own risk.
- Do not use modified game binaries/files in contexts where they may violate a game's terms or online play rules.

See also:

```text
docs/SAFETY_AND_LEGAL.md
THIRD_PARTY_NOTICES.md
```

## License

GPL-3.0 is recommended for public distribution of this repository. Add a `LICENSE` file before publishing.

This project was developed with reference to `fh6-universal-radio`. See `THIRD_PARTY_NOTICES.md`.
