using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Security.Principal;

namespace TestReceiver.IPC;

public sealed class SharedAudioReader : IDisposable
{
    private const int TargetLatencyFrames = SharedAudioProtocol.SampleRate / 5;
    private readonly object _gate = new();
    private MemoryMappedFile? _memoryMappedFile;
    private MemoryMappedViewAccessor? _accessor;
    private string? _sharedMemoryName;
    private DateTimeOffset _nextConnectAttemptUtc = DateTimeOffset.MinValue;
    private ulong _readFrame;
    private ulong _localUnderrunCount;
    private string _lastError = string.Empty;
    private bool _canWriteReadFrame;

    public int Read(byte[] buffer, int offset, int count)
    {
        var requestedFrames = count / SharedAudioProtocol.FrameBytes;
        var usableBytes = requestedFrames * SharedAudioProtocol.FrameBytes;

        if (usableBytes <= 0)
        {
            Array.Clear(buffer, offset, count);
            return count;
        }

        lock (_gate)
        {
            if (!TryEnsureConnected() || _accessor is null || !SharedAudioProtocol.IsHeaderValid(_accessor))
            {
                Array.Clear(buffer, offset, count);
                return count;
            }

            try
            {
                var writeFrame = _accessor.ReadUInt64(SharedAudioProtocol.WriteFrameOffset);
                var lastWriteQpc = _accessor.ReadUInt64(SharedAudioProtocol.LastWriteQpcOffset);

                if (IsWriterStale(lastWriteQpc))
                {
                    _localUnderrunCount++;
                    Array.Clear(buffer, offset, count);
                    return count;
                }

                if (_readFrame == 0 || _readFrame > writeFrame)
                {
                    _readFrame = writeFrame > TargetLatencyFrames ? writeFrame - TargetLatencyFrames : writeFrame;
                }

                if (writeFrame >= _readFrame &&
                    writeFrame - _readFrame > SharedAudioProtocol.BufferFrames - TargetLatencyFrames)
                {
                    _readFrame = writeFrame > TargetLatencyFrames ? writeFrame - TargetLatencyFrames : writeFrame;
                }

                var availableFrames = writeFrame >= _readFrame ? writeFrame - _readFrame : 0;
                if (availableFrames == 0)
                {
                    _localUnderrunCount++;
                    Array.Clear(buffer, offset, count);
                    return count;
                }

                var framesToRead = (int)Math.Min((ulong)requestedFrames, availableFrames);
                ReadFrames(buffer, offset, framesToRead);

                var bytesRead = framesToRead * SharedAudioProtocol.FrameBytes;
                if (bytesRead < count)
                {
                    _localUnderrunCount++;
                    Array.Clear(buffer, offset + bytesRead, count - bytesRead);
                }

                _readFrame += (ulong)framesToRead;
                TryPublishReadFrame();
                _lastError = string.Empty;
                return count;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ObjectDisposedException)
            {
                _lastError = ex.Message;
                Disconnect();
                Array.Clear(buffer, offset, count);
                return count;
            }
        }
    }

    public ReaderDebugState GetDebugState()
    {
        lock (_gate)
        {
            _ = TryEnsureConnected();

            if (_accessor is null || !SharedAudioProtocol.IsHeaderValid(_accessor))
            {
                return new ReaderDebugState(
                    false,
                    _sharedMemoryName,
                    0,
                    _readFrame,
                    0,
                    _localUnderrunCount,
                    0,
                    0,
                    _lastError);
            }

            var writeFrame = _accessor.ReadUInt64(SharedAudioProtocol.WriteFrameOffset);
            var latencyFrames = writeFrame >= _readFrame ? writeFrame - _readFrame : 0;

            return new ReaderDebugState(
                true,
                _sharedMemoryName,
                writeFrame,
                _readFrame,
                (long)(latencyFrames * 1000 / SharedAudioProtocol.SampleRate),
                _localUnderrunCount,
                _accessor.ReadSingle(SharedAudioProtocol.PeakLOffset),
                _accessor.ReadSingle(SharedAudioProtocol.PeakROffset),
                _lastError);
        }
    }

    private bool TryEnsureConnected()
    {
        if (_accessor is not null)
        {
            return true;
        }

        if (DateTimeOffset.UtcNow < _nextConnectAttemptUtc)
        {
            return false;
        }

        _nextConnectAttemptUtc = DateTimeOffset.UtcNow.AddSeconds(1);

        foreach (var name in GetNameCandidates())
        {
            if (TryConnect(name, writable: true) || TryConnect(name, writable: false))
            {
                _sharedMemoryName = name;
                _lastError = string.Empty;
                return true;
            }
        }

        _lastError = "waiting for bridge";
        return false;
    }

    private bool TryConnect(string name, bool writable)
    {
        try
        {
            var rights = writable ? MemoryMappedFileRights.ReadWrite : MemoryMappedFileRights.Read;
            var access = writable ? MemoryMappedFileAccess.ReadWrite : MemoryMappedFileAccess.Read;
            var mmf = MemoryMappedFile.OpenExisting(name, rights);
            var accessor = mmf.CreateViewAccessor(0, SharedAudioProtocol.TotalBytes, access);

            if (!SharedAudioProtocol.IsHeaderValid(accessor))
            {
                accessor.Dispose();
                mmf.Dispose();
                return false;
            }

            _memoryMappedFile = mmf;
            _accessor = accessor;
            _canWriteReadFrame = writable;
            _readFrame = 0;
            _sharedMemoryName = name;
            Console.WriteLine($"Connected to shared memory: {name}");
            return true;
        }
        catch (Exception ex) when (ex is FileNotFoundException or UnauthorizedAccessException or IOException)
        {
            return false;
        }
    }

    private static IEnumerable<string> GetNameCandidates()
    {
        if (IsProcessElevated())
        {
            yield return SharedAudioProtocol.GlobalMemoryName;
        }

        yield return SharedAudioProtocol.LocalMemoryName;
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

    private void ReadFrames(byte[] destination, int destinationOffset, int frameCount)
    {
        if (_accessor is null)
        {
            return;
        }

        var remainingFrames = frameCount;
        var currentDestinationOffset = destinationOffset;
        var currentReadFrame = _readFrame;

        while (remainingFrames > 0)
        {
            var ringFrame = currentReadFrame % SharedAudioProtocol.BufferFrames;
            var framesUntilWrap = SharedAudioProtocol.BufferFrames - ringFrame;
            var framesThisRead = (int)Math.Min((ulong)remainingFrames, framesUntilWrap);
            var sourcePosition = SharedAudioProtocol.PcmDataOffset + ((long)ringFrame * SharedAudioProtocol.FrameBytes);
            var bytesThisRead = framesThisRead * SharedAudioProtocol.FrameBytes;

            _accessor.ReadArray(sourcePosition, destination, currentDestinationOffset, bytesThisRead);

            currentDestinationOffset += bytesThisRead;
            currentReadFrame += (ulong)framesThisRead;
            remainingFrames -= framesThisRead;
        }
    }

    private void TryPublishReadFrame()
    {
        if (!_canWriteReadFrame || _accessor is null)
        {
            return;
        }

        try
        {
            _accessor.Write(SharedAudioProtocol.ReadFrameOffset, _readFrame);
        }
        catch
        {
            _canWriteReadFrame = false;
        }
    }

    private static bool IsWriterStale(ulong lastWriteQpc)
    {
        if (lastWriteQpc == 0)
        {
            return true;
        }

        var delta = Stopwatch.GetTimestamp() - (long)lastWriteQpc;
        return delta < 0 || delta > Stopwatch.Frequency * 2;
    }

    private void Disconnect()
    {
        _accessor?.Dispose();
        _memoryMappedFile?.Dispose();
        _accessor = null;
        _memoryMappedFile = null;
        _canWriteReadFrame = false;
        _sharedMemoryName = null;
        _readFrame = 0;
    }

    public void Dispose()
    {
        lock (_gate)
        {
            Disconnect();
        }
    }
}
