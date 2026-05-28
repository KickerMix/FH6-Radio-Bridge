using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Security.Principal;
using FH6RadioBridge.Audio;
using FH6RadioBridge.Logging;

namespace FH6RadioBridge.IPC;

public sealed class SharedAudioWriter : IDisposable
{
    private readonly object _gate = new();
    private readonly MemoryMappedFile _memoryMappedFile;
    private readonly MemoryMappedViewAccessor _accessor;
    private readonly EventWaitHandle? _event;
    private readonly bool _useLimiter;
    private ulong _writeFrame;
    private ulong _underrunCount;
    private float _volume;
    private AudioLevels _lastLevels;
    private bool _disposed;

    private SharedAudioWriter(
        string sharedMemoryName,
        MemoryMappedFile memoryMappedFile,
        MemoryMappedViewAccessor accessor,
        EventWaitHandle? readyEvent,
        float initialVolume,
        bool useLimiter)
    {
        SharedMemoryName = sharedMemoryName;
        _memoryMappedFile = memoryMappedFile;
        _accessor = accessor;
        _event = readyEvent;
        _volume = initialVolume;
        _useLimiter = useLimiter;

        InitializeHeader();
    }

    public string SharedMemoryName { get; }

    public static SharedAudioWriter Create(float initialVolume, bool useLimiter)
    {
        Exception? lastError = null;

        foreach (var candidate in GetNameCandidates())
        {
            try
            {
                var mmf = MemoryMappedFile.CreateOrOpen(
                    candidate.MemoryName,
                    SharedAudioProtocol.TotalBytes,
                    MemoryMappedFileAccess.ReadWrite);
                var accessor = mmf.CreateViewAccessor(
                    0,
                    SharedAudioProtocol.TotalBytes,
                    MemoryMappedFileAccess.ReadWrite);
                var readyEvent = TryCreateEvent(candidate.EventName);

                Log.Info($"Shared memory ready: {candidate.MemoryName}");
                if (readyEvent is not null)
                {
                    Log.Info($"Shared event ready: {candidate.EventName}");
                }

                return new SharedAudioWriter(candidate.MemoryName, mmf, accessor, readyEvent, initialVolume, useLimiter);
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                lastError = ex;
                Log.Warn($"Could not create shared memory {candidate.MemoryName}: {ex.Message}");
            }
        }

        throw new InvalidOperationException("Could not create Global or Local shared memory.", lastError);
    }

    public void SetVolume(float volume)
    {
        lock (_gate)
        {
            _volume = Math.Clamp(volume, 0.0f, 2.0f);
            _accessor.Write(SharedAudioProtocol.VolumeOffset, _volume);
        }
    }

    public void WriteSamples(float[] samples, int sampleCount)
    {
        if (sampleCount < SharedAudioProtocol.Channels)
        {
            return;
        }

        lock (_gate)
        {
            ThrowIfDisposed();

            var frameCount = sampleCount / SharedAudioProtocol.Channels;
            var sampleOffset = 0;

            if (frameCount > SharedAudioProtocol.BufferFrames)
            {
                var framesToSkip = frameCount - SharedAudioProtocol.BufferFrames;
                sampleOffset = framesToSkip * SharedAudioProtocol.Channels;
                frameCount = SharedAudioProtocol.BufferFrames;
            }

            var samplesToWrite = frameCount * SharedAudioProtocol.Channels;
            ApplyVolumeAndLimiter(samples, sampleOffset, samplesToWrite);
            _lastLevels = AudioLevelMeter.Calculate(samples, sampleOffset, samplesToWrite, SharedAudioProtocol.Channels);
            WritePcmToRing(samples, sampleOffset, frameCount);

            _writeFrame += (ulong)frameCount;
            UpdateHeaderAfterWrite();
            _event?.Set();
        }
    }

    public SharedAudioSnapshot GetSnapshot()
    {
        lock (_gate)
        {
            return new SharedAudioSnapshot(
                SharedMemoryName,
                SharedAudioProtocol.IsHeaderValid(_accessor),
                _accessor.ReadUInt32(SharedAudioProtocol.FlagsOffset),
                _accessor.ReadUInt64(SharedAudioProtocol.WriteFrameOffset),
                _accessor.ReadUInt64(SharedAudioProtocol.ReadFrameOffset),
                _accessor.ReadUInt64(SharedAudioProtocol.LastWriteQpcOffset),
                _accessor.ReadUInt64(SharedAudioProtocol.UnderrunCountOffset),
                _accessor.ReadSingle(SharedAudioProtocol.VolumeOffset),
                _accessor.ReadSingle(SharedAudioProtocol.PeakLOffset),
                _accessor.ReadSingle(SharedAudioProtocol.PeakROffset),
                _accessor.ReadSingle(SharedAudioProtocol.RmsLOffset),
                _accessor.ReadSingle(SharedAudioProtocol.RmsROffset));
        }
    }

    private void InitializeHeader()
    {
        _accessor.Write(SharedAudioProtocol.MagicOffset, SharedAudioProtocol.Magic);
        _accessor.Write(SharedAudioProtocol.VersionOffset, SharedAudioProtocol.Version);
        _accessor.Write(SharedAudioProtocol.HeaderSizeOffset, (uint)SharedAudioProtocol.HeaderSize);
        _accessor.Write(SharedAudioProtocol.SampleRateOffset, (uint)SharedAudioProtocol.SampleRate);
        _accessor.Write(SharedAudioProtocol.ChannelsOffset, (uint)SharedAudioProtocol.Channels);
        _accessor.Write(SharedAudioProtocol.FormatOffset, SharedAudioProtocol.FormatFloat32Interleaved);
        _accessor.Write(SharedAudioProtocol.BufferFramesOffset, (uint)SharedAudioProtocol.BufferFrames);
        _accessor.Write(SharedAudioProtocol.FlagsOffset, (uint)AudioSharedFlags.SourceAvailable);
        _accessor.Write(SharedAudioProtocol.WriteFrameOffset, 0UL);
        _accessor.Write(SharedAudioProtocol.ReadFrameOffset, 0UL);
        _accessor.Write(SharedAudioProtocol.LastWriteQpcOffset, 0UL);
        _accessor.Write(SharedAudioProtocol.UnderrunCountOffset, 0UL);
        _accessor.Write(SharedAudioProtocol.VolumeOffset, _volume);
        _accessor.Write(SharedAudioProtocol.PeakLOffset, 0f);
        _accessor.Write(SharedAudioProtocol.PeakROffset, 0f);
        _accessor.Write(SharedAudioProtocol.RmsLOffset, 0f);
        _accessor.Write(SharedAudioProtocol.RmsROffset, 0f);

        var clearBuffer = new byte[64 * 1024];
        long remaining = SharedAudioProtocol.BufferBytes;
        long offset = SharedAudioProtocol.PcmDataOffset;
        while (remaining > 0)
        {
            var count = (int)Math.Min(clearBuffer.Length, remaining);
            _accessor.WriteArray(offset, clearBuffer, 0, count);
            remaining -= count;
            offset += count;
        }
    }

    private void ApplyVolumeAndLimiter(float[] samples, int offset, int sampleCount)
    {
        var volume = _volume;

        for (var i = 0; i < sampleCount; i++)
        {
            var value = samples[offset + i] * volume;
            samples[offset + i] = _useLimiter ? Math.Clamp(value, -1.0f, 1.0f) : value;
        }
    }

    private void WritePcmToRing(float[] samples, int sampleOffset, int frameCount)
    {
        var remainingFrames = frameCount;
        var currentSampleOffset = sampleOffset;
        var currentWriteFrame = _writeFrame;

        while (remainingFrames > 0)
        {
            var ringFrame = (long)(currentWriteFrame % SharedAudioProtocol.BufferFrames);
            var framesUntilWrap = SharedAudioProtocol.BufferFrames - ringFrame;
            var framesThisWrite = (int)Math.Min(remainingFrames, framesUntilWrap);
            var position = SharedAudioProtocol.PcmDataOffset + (ringFrame * SharedAudioProtocol.FrameBytes);
            var samplesThisWrite = framesThisWrite * SharedAudioProtocol.Channels;

            _accessor.WriteArray(position, samples, currentSampleOffset, samplesThisWrite);

            currentSampleOffset += samplesThisWrite;
            remainingFrames -= framesThisWrite;
            currentWriteFrame += (ulong)framesThisWrite;
        }
    }

    private void UpdateHeaderAfterWrite()
    {
        var flags = AudioSharedFlags.SourceAvailable;
        if (_volume <= 0.0001f)
        {
            flags |= AudioSharedFlags.Muted;
        }

        if (_lastLevels.RmsL > 0.0001f || _lastLevels.RmsR > 0.0001f)
        {
            flags |= AudioSharedFlags.Playing;
        }

        var readFrame = _accessor.ReadUInt64(SharedAudioProtocol.ReadFrameOffset);
        if (readFrame > 0 && _writeFrame > readFrame && _writeFrame - readFrame > SharedAudioProtocol.BufferFrames)
        {
            _underrunCount++;
        }

        _accessor.Write(SharedAudioProtocol.FlagsOffset, (uint)flags);
        _accessor.Write(SharedAudioProtocol.VolumeOffset, _volume);
        _accessor.Write(SharedAudioProtocol.PeakLOffset, _lastLevels.PeakL);
        _accessor.Write(SharedAudioProtocol.PeakROffset, _lastLevels.PeakR);
        _accessor.Write(SharedAudioProtocol.RmsLOffset, _lastLevels.RmsL);
        _accessor.Write(SharedAudioProtocol.RmsROffset, _lastLevels.RmsR);
        _accessor.Write(SharedAudioProtocol.UnderrunCountOffset, _underrunCount);
        _accessor.Write(SharedAudioProtocol.LastWriteQpcOffset, (ulong)Stopwatch.GetTimestamp());
        _accessor.Write(SharedAudioProtocol.WriteFrameOffset, _writeFrame);
    }

    private static EventWaitHandle? TryCreateEvent(string eventName)
    {
        try
        {
            return new EventWaitHandle(false, EventResetMode.AutoReset, eventName);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
        {
            Log.Warn($"Could not create shared event {eventName}: {ex.Message}");
            return null;
        }
    }

    private static IEnumerable<(string MemoryName, string EventName)> GetNameCandidates()
    {
        if (IsProcessElevated())
        {
            yield return (SharedAudioProtocol.GlobalMemoryName, SharedAudioProtocol.GlobalEventName);
        }
        else
        {
            Log.Warn("Global shared memory namespace skipped because the process is not elevated; using Local fallback.");
        }

        yield return (SharedAudioProtocol.LocalMemoryName, SharedAudioProtocol.LocalEventName);
    }

    private static bool IsProcessElevated()
    {
        try
        {
            using var identity = WindowsIdentity.GetCurrent();
            var principal = new WindowsPrincipal(identity);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch
        {
            return false;
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        try
        {
            _accessor.Write(SharedAudioProtocol.FlagsOffset, (uint)AudioSharedFlags.None);
        }
        catch
        {
            // Ignore shutdown races.
        }

        _event?.Dispose();
        _accessor.Dispose();
        _memoryMappedFile.Dispose();
    }
}
