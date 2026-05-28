namespace FH6RadioBridge.Metadata;

public sealed class MetadataState
{
    private MetadataSnapshot _current = MetadataSnapshot.Unknown();

    public MetadataSnapshot Current => _current;

    public void Update(MetadataSnapshot snapshot)
    {
        _current = snapshot;
    }

    public static MetadataState Unknown() => new();
}

public sealed record MetadataSnapshot(
    string Title,
    string Artist,
    string Album,
    string PlaybackStatus,
    string SourceAppUserModelId,
    DateTimeOffset UpdatedAtUtc)
{
    public static MetadataSnapshot Unknown(string playbackStatus = "Unknown") => new(
        "Unknown",
        "Unknown",
        "Unknown",
        playbackStatus,
        "Unknown",
        DateTimeOffset.UtcNow);
}

public sealed record MediaControlResult(
    bool Supported,
    bool Sent,
    string Command,
    string Reason);
