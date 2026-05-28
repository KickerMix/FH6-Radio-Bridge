# Architecture

## Current scope

The current implementation has a working external audio bridge and a controlled game-side proxy probe:

```text
Audio source
  -> NAudio capture
  -> Media Foundation resampler
  -> 48000 Hz stereo float32 PCM
  -> shared memory ring buffer
  -> TestReceiver playback, XAudio2 in-process probe, or future game-side radio callback reader

Game process
  -> loads game-local version.dll proxy
  -> proxy forwards system version.dll exports
  -> proxy logs process/shared-memory status
  -> optional XAudio2 source voice reads shared-memory PCM
```

The current goal is to validate stable audio transport and safe DLL loading before any FMOD or asset modification work.

## Components

### FH6 Radio Bridge

`FH6RadioBridge.exe` is a console application. It can list capture devices, capture a selected input device, or capture the default render endpoint via WASAPI loopback. It converts all input to the internal format and writes frames to shared memory.

It also hosts a local Kestrel API on `127.0.0.1:8420`:

- `GET /api/state`
- `GET /api/metadata`
- `POST /api/settings/volume`
- `POST /api/control/playpause`
- `POST /api/control/next`
- `POST /api/control/previous`

Metadata and media control endpoints use Windows Global System Media Transport Controls. They are best-effort and return `Unknown` or a rejected command result if an application does not expose a compatible media session.

### TestReceiver

`TestReceiver.exe` opens the shared memory block, validates the header, reads PCM frames from the ring buffer, and plays them on the default output device. If the bridge is absent, stale, or invalid, it returns silence.

### Game-side DLL proxy

The `version.dll` proxy is a fail-safe game-side probe. It loads only `C:\Windows\System32\version.dll`, forwards the exported version APIs, writes a local log next to the proxy DLL, and probes shared memory status. It does not install hooks, call remote APIs, make network requests, or patch game memory.

The install script copies only `hook\build\Release\version.dll` into the selected game directory and writes `fh6-radio-bridge\install_manifest.json`. Existing `version.dll` files are not overwritten unless `-Force` is provided, and they are backed up before replacement.

### XAudio2 in-process audio probe

The optional XAudio2 probe is enabled by `fh6-radio-bridge\enable_inprocess_audio.flag`. When enabled, the proxy starts a worker thread, creates its own XAudio2 mastering/source voice, and submits 48000 Hz stereo float32 buffers read from shared memory. Missing, stale, or invalid shared memory produces silence.

Playback is additionally controlled by `fh6-radio-bridge\radio_active.flag`. Without that flag, the worker stays alive and submits silence. With that flag, it reads PCM from shared memory. The same gate can be toggled with `F8` while the game window is focused. This separates "player exists in the game process" from "radio is active" and gives the future Station 10 detector a narrow control surface.

This is not final radio integration. It proves that the injected DLL can render the bridge audio from inside the game process without patching game memory or replacing FMOD banks.

### Future FMOD path

FMOD integration is still not attached to the game. The current `FmodRadioPcmProvider` and `SharedAudioRingReader` are game-independent callback-shaped code that read float32 stereo PCM from shared memory and return silence on failure.

## Spotify reference observations

The `ExampleHookForzaHorizon6butForSpotify` folder was used as architecture reference only. The useful observations are:

- It uses game-directory `version.dll` loading for injection.
- Its `version.dll` export table exposes the same 17 system `version.dll` exports and forwards them to `C:\Windows\System32\version`.
- It modifies station 10, `Streamer Mode`, through localized `RadioInfo_*.xml` files and expects `Streamer Mode = On`.
- It adds replacement FMOD bank files and UI assets.
- Its dashboard checks states equivalent to game attached, injector ready, audio active, station selected, and service connected.

No binaries, FMOD banks, UI assets, textures, or XML payloads from the reference package are copied into this repository.

## Failure behavior

- Bridge logs capture and shared memory setup errors.
- Receiver survives bridge absence and restart.
- Invalid headers are rejected.
- Stale writers produce silence instead of stale audio.
- HTTP API binds only to loopback.
- The DLL proxy logs and continues if shared memory is missing or invalid.
- The XAudio2 probe is disabled unless its flag file exists and returns silence on bridge failure.
- The live radio gate defaults to inactive, so the player can be present without always playing over game audio.
- The install/uninstall scripts refuse to run while a process from the target directory is active.
