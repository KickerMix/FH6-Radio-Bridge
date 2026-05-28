using System.IO.MemoryMappedFiles;

namespace TestReceiver.IPC;

[Flags]
public enum AudioSharedFlags : uint
{
    None = 0,
    Playing = 1 << 0,
    Muted = 1 << 1,
    SourceAvailable = 1 << 2
}

public static class SharedAudioProtocol
{
    public const string GlobalMemoryName = "Global\\FH6RadioBridgeAudio";
    public const string LocalMemoryName = "Local\\FH6RadioBridgeAudio";
    public const uint Magic = 0x52484659;
    public const uint Version = 1;
    public const uint FormatFloat32Interleaved = 1;
    public const int SampleRate = 48000;
    public const int Channels = 2;
    public const int BufferSeconds = 5;
    public const int BufferFrames = SampleRate * BufferSeconds;
    public const int BytesPerSample = sizeof(float);
    public const int FrameBytes = Channels * BytesPerSample;
    public const int BufferSamples = BufferFrames * Channels;
    public const int BufferBytes = BufferSamples * BytesPerSample;
    public const int HeaderSize = 84;
    public const long TotalBytes = HeaderSize + BufferBytes;

    public const long MagicOffset = 0;
    public const long VersionOffset = 4;
    public const long HeaderSizeOffset = 8;
    public const long SampleRateOffset = 12;
    public const long ChannelsOffset = 16;
    public const long FormatOffset = 20;
    public const long BufferFramesOffset = 24;
    public const long FlagsOffset = 28;
    public const long WriteFrameOffset = 32;
    public const long ReadFrameOffset = 40;
    public const long LastWriteQpcOffset = 48;
    public const long UnderrunCountOffset = 56;
    public const long VolumeOffset = 64;
    public const long PeakLOffset = 68;
    public const long PeakROffset = 72;
    public const long RmsLOffset = 76;
    public const long RmsROffset = 80;
    public const long PcmDataOffset = HeaderSize;

    public static readonly string[] MemoryNameCandidates = [GlobalMemoryName, LocalMemoryName];

    public static bool IsHeaderValid(MemoryMappedViewAccessor accessor)
    {
        try
        {
            return accessor.ReadUInt32(MagicOffset) == Magic &&
                   accessor.ReadUInt32(VersionOffset) == Version &&
                   accessor.ReadUInt32(HeaderSizeOffset) == HeaderSize &&
                   accessor.ReadUInt32(SampleRateOffset) == SampleRate &&
                   accessor.ReadUInt32(ChannelsOffset) == Channels &&
                   accessor.ReadUInt32(FormatOffset) == FormatFloat32Interleaved &&
                   accessor.ReadUInt32(BufferFramesOffset) == BufferFrames;
        }
        catch
        {
            return false;
        }
    }
}

public sealed record ReaderDebugState(
    bool Connected,
    string? SharedMemoryName,
    ulong WriteFrame,
    ulong ReadFrame,
    long LatencyMilliseconds,
    ulong LocalUnderrunCount,
    float PeakL,
    float PeakR,
    string LastError);
