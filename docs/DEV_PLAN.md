# Development Plan

## Phase 0

Done in this repository:

- Repository structure.
- README and IPC documentation.
- Build and run scripts.
- .NET solution with bridge and receiver projects.

## Phase 1

Done in this repository:

- Device listing.
- Capture from selected input device.
- Optional default WASAPI loopback capture.
- Conversion to 48000 Hz stereo float32.
- Shared memory writer.
- Local `/api/state`.

## Phase 2

Done in this repository:

- Shared memory reader.
- Default output playback.
- Silence on missing or invalid bridge.
- Debug logging for connection and latency.

## Phase 3

Done in this repository:

- Windows Global System Media Transport Controls metadata polling.
- `/api/metadata`.
- Media session logging.
- Local `playpause`, `next`, and `previous` control endpoints.

## Phase 4

Done in this repository:

- `version.dll` proxy target.
- Forwarding exports for the system `version.dll` surface used by the task.
- Real DLL loaded from `C:\Windows\System32\version.dll`.
- Log file at `fh6-radio-bridge/logs/hook.log`, with `%LOCALAPPDATA%/FH6RadioBridge/hook.log` fallback.
- Shared memory status probe with silence-safe failure behavior.

## Phase 5

Done in this repository:

- `FmodRadioPcmProvider` shaped like a future FMOD float32 stereo read callback.
- `SharedAudioRingReader` that maps shared memory outside the callback path.
- `SharedAudioReadTest.exe` game-independent harness.
- Callback design notes in `docs/FMOD_CALLBACK_PLAN.md`.

## Phase 6

Done in this repository:

- Safe game-side proxy install script.
- Safe game-side proxy uninstall script with optional backup restore.
- Hook log viewer script.
- DLL startup logging for process id, process image path, proxy DLL path, current directory, system `version.dll` load status, and shared memory status.
- Reference-mod architecture notes based on text/XML/PE metadata inspection only.

## Phase 7

Done in this repository:

- Optional in-process XAudio2 shared audio player.
- `enable_inprocess_audio.ps1` and `disable_inprocess_audio.ps1` flag scripts.
- Fail-safe silence when shared memory is missing, stale, or invalid.
- Worker status logging in `fh6-radio-bridge/logs/hook.log`.
- Live `activate_radio_audio.ps1` / `deactivate_radio_audio.ps1` gate scripts.
- In-game focused-window `F8` toggle for the same gate.
- Safe Station 10 `RadioInfo_*.xml` metadata install/uninstall scripts with backup manifest.
- Experimental Station 10 replacement-mode XML patch using a synthetic missing sound id to suppress native station music without FMOD bank files.

## Next phases

1. Install the Phase 7 DLL after the game is closed.
2. Enable the XAudio2 flag, restart bridge/game, and verify inactive-by-default gate behavior.
3. Add game build/version detection before any FMOD attachment.
4. Prototype Station 10/Streamer Mode detection.
5. If synthetic missing-sound replacement is rejected by the game, evaluate local FMOD bank generation using original tooling/assets only, not reference mod banks.

Future FMOD integration should remain behind explicit version/signature checks and should not patch arbitrary game memory or use hardcoded offsets in the main branch.
