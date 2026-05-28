# Safety and Legal Notes

This project does not bypass audio-service DRM.
This project does not download or decrypt tracks from any service.
This project captures audio that is already playing on the user's system.
This project does not include any files from Spotify Radio mod or Forza Horizon 6.

The bridge has no anti-cheat bypass, no memory patching, and no remote telemetry. The local HTTP API binds to `127.0.0.1` only.

The game-side `version.dll` proxy only forwards system version APIs, logs status, and reads shared memory state. It does not access network services, game online systems, remote APIs, or DRM-protected data.

The optional XAudio2 in-process probe is disabled by default and enabled only by a local flag file. It creates its own XAudio2 voice and plays already-captured PCM from shared memory; it does not modify game memory, bypass DRM, or replace game asset files.

The install script copies only this repository's generated `version.dll` proxy into the selected game directory. It does not install copied Spotify reference assets, FMOD banks, textures, localized XML, credentials, or UI bundles.

The Station 10 metadata script edits only the user's local `RadioInfo_*.xml` files and writes backups before changing them. It does not copy XML payloads from the Spotify reference package and does not modify FMOD bank files.

Use the proxy stage for single-player/offline testing only and uninstall it before online play.
