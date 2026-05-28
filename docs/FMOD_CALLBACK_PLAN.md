# FMOD Callback Plan

## Current Phase 5 Scope

The repository contains a game-independent PCM provider shaped like a future FMOD stream callback:

```text
Shared memory PCM ring buffer
  -> SharedAudioRingReader
  -> FmodRadioPcmProvider::FillFloat32Stereo
  -> SharedAudioReadTest.exe
```

This does not hook the game, patch memory, call FMOD, or use hardcoded offsets. It only validates that C++ code can read the same 48000 Hz stereo float32 stream that `TestReceiver.exe` uses.

## Callback Constraints

A future FMOD callback must:

- perform no allocation;
- perform no file IO;
- perform no HTTP or network calls;
- avoid blocking waits and heavy locks;
- copy PCM from already-mapped shared memory or return silence;
- treat invalid headers, stale writers, and missing data as silence.

## Reader Behavior

`SharedAudioRingReader::TryConnect()` opens and maps shared memory outside the callback path. After connection, `ReadFrames()` only reads the fixed header, checks freshness, copies PCM with `memcpy`, and fills missing frames with zeroes.

If the writer is stale for more than 2 seconds, if the header is invalid, or if not enough frames are available, the reader returns silence for the unavailable part.

## Reference Mod Notes

The local `ExampleHookForzaHorizon6butForSpotify` folder was used only as architectural context. The observed text-level pattern is:

- `version.dll` proxy in the game directory;
- patched localized `media/Audio/RadioInfo_*.xml`;
- replacement `R9_Tracks_CU1.bank` and `R9_Tracks_CU1.assets.bank` files;
- rebranded R10/Streamer Mode station;
- Station 10 uses the existing `HZ6_R9_PeterBroderick_EyesClosedandTraveling` sound name as a stable anchor instead of inventing a new sound name;
- the DLL strings indicate runtime discovery of FMOD `createSound`/`playSound`, an FMOD stream object, R10 active detection, and migration to the game's radio channel group;
- local dashboard.

No code, binaries, banks, UI assets, textures, or XML contents from that mod are copied into this project.

## Probe Before Hooking

The repository now includes a gated read-only memory probe. It exists to answer three questions before any patching or function calls are attempted:

- Which loaded module contains FMOD/static audio code and radio strings?
- Are Station 10 XML strings and bank identifiers visible in module memory or only in runtime heap/mapped memory?
- Can we identify a stable, version-checked anchor for future FMOD `createSound`/`playSound` attachment?

The probe logs module inventory and capped pattern hits only. It does not patch memory, call FMOD, or alter audio routing.
