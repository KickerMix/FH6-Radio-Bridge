# Shared Memory IPC Protocol

## Names

Elevated writer processes try:

```text
Global\FH6RadioBridgeAudio
Global\FH6RadioBridgeAudioEvent
```

If global names are unavailable, or if the process is not elevated, it falls back to:

```text
Local\FH6RadioBridgeAudio
Local\FH6RadioBridgeAudioEvent
```

## Audio format

```text
Sample rate: 48000 Hz
Channels: 2
Format: float32 interleaved
Layout: L R L R ...
```

## Ring buffer

```text
bufferSeconds = 5
bufferFrames = 48000 * 5 = 240000
bufferSamples = 240000 * 2 = 480000
bufferBytes = 480000 * 4 = 1920000
```

## Header

All fields use little-endian native Windows layout. The header is manually written at fixed offsets and is 84 bytes.

| Offset | Type | Name | Description |
| ---: | --- | --- | --- |
| 0 | uint32 | magic | `0x52484659`, ASCII `YFHR` |
| 4 | uint32 | version | `1` |
| 8 | uint32 | headerSize | `84` |
| 12 | uint32 | sampleRate | `48000` |
| 16 | uint32 | channels | `2` |
| 20 | uint32 | format | `1`, float32 interleaved |
| 24 | uint32 | bufferFrames | `240000` |
| 28 | uint32 | flags | bit 0 playing, bit 1 muted, bit 2 source available |
| 32 | uint64 | writeFrame | Monotonic absolute frame counter |
| 40 | uint64 | readFrame | Receiver debug counter |
| 48 | uint64 | lastWriteQpc | `Stopwatch.GetTimestamp()` from writer |
| 56 | uint64 | underrunCount | Writer-side/debug counter |
| 64 | float | volume | Writer gain |
| 68 | float | peakL | Recent left peak |
| 72 | float | peakR | Recent right peak |
| 76 | float | rmsL | Recent left RMS |
| 80 | float | rmsR | Recent right RMS |

PCM data begins immediately at offset 84.

## Reader rules

Readers must:

1. Validate `magic`, `version`, `headerSize`, `sampleRate`, `channels`, `format`, and `bufferFrames`.
2. Keep their own local `readFrame`.
3. Output silence when the writer is absent, stale, or invalid.
4. Jump near `writeFrame` if too far behind.
5. Never trust the shared memory header enough to crash.
