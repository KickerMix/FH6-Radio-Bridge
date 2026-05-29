using Windows.Foundation;
using Windows.Media.Control;
using FH6RadioBridge.Config;
using FH6RadioBridge.Logging;

namespace FH6RadioBridge.Metadata;

public sealed class MediaSessionMetadataService : IAsyncDisposable
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);
    private readonly MetadataState _state;
    private readonly AppConfig _config;
    private readonly CancellationTokenSource _stop = new();
    private GlobalSystemMediaTransportControlsSessionManager? _manager;
    private Task? _pollingTask;
    private string _lastSessionSignature = string.Empty;
    private string _lastManualFallbackWarning = string.Empty;

    public MediaSessionMetadataService(MetadataState state, AppConfig config)
    {
        _state = state;
        _config = config;
    }

    public bool IsAvailable => _manager is not null;

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows())
        {
            Log.Warn("Windows media metadata is unavailable on this OS.");
            _state.Update(MetadataSnapshot.Unknown("Unavailable"));
            return;
        }

        try
        {
            _manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            Log.Info("Windows media session metadata service started.");
            await RefreshAsync(cancellationToken);
            _pollingTask = Task.Run(() => PollAsync(_stop.Token), CancellationToken.None);
        }
        catch (Exception ex)
        {
            Log.Warn($"Windows media session metadata is unavailable: {ex.Message}");
            _state.Update(MetadataSnapshot.Unknown("Unavailable"));
        }
    }

    public Task<MediaControlResult> TryPlayPauseAsync() =>
        TryControlAsync("playpause", session => session.TryTogglePlayPauseAsync());

    public Task<MediaControlResult> TryNextAsync() =>
        TryControlAsync("next", session => session.TrySkipNextAsync());

    public Task<MediaControlResult> TryPreviousAsync() =>
        TryControlAsync("previous", session => session.TrySkipPreviousAsync());

    public Task<MediaControlResult> TryRestartCurrentAsync() =>
        TryControlAsync("restart", session => session.TryChangePlaybackPositionAsync(0));

    public async Task<MediaSessionsResponse> GetSessionsAsync()
    {
        var manager = _manager;
        if (manager is null)
        {
            return new MediaSessionsResponse(
                _config.Metadata.SessionSelectionMode.ToString(),
                _config.Metadata.PreferredSourceAppUserModelId,
                string.Empty,
                []);
        }

        var sessions = manager.GetSessions();
        var selected = GetSelectedSession();
        var selectedSource = selected?.SourceAppUserModelId ?? string.Empty;
        var result = new List<MediaSessionInfo>();

        foreach (var session in sessions)
        {
            var status = GetStatus(session).ToString();
            var title = "Unknown";
            var artist = "Unknown";
            var album = "Unknown";

            try
            {
                var props = await session.TryGetMediaPropertiesAsync();
                title = Normalize(props.Title);
                artist = Normalize(props.Artist);
                album = Normalize(props.AlbumTitle);
            }
            catch
            {
                // Keep session listing best-effort.
            }

            result.Add(new MediaSessionInfo(
                Normalize(session.SourceAppUserModelId),
                status,
                title,
                artist,
                album,
                selected is not null && SameSource(session, selected)));
        }

        return new MediaSessionsResponse(
            _config.Metadata.SessionSelectionMode.ToString(),
            _config.Metadata.PreferredSourceAppUserModelId,
            selectedSource,
            result);
    }

    private async Task PollAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(PollInterval);

        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken))
            {
                await RefreshSafeAsync(cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
            // Expected during shutdown.
        }
    }

    private async Task RefreshSafeAsync(CancellationToken cancellationToken)
    {
        try
        {
            await RefreshAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log.Warn($"Could not refresh Windows media metadata: {ex.Message}");
            _state.Update(MetadataSnapshot.Unknown("Unknown"));
        }
    }

    private async Task RefreshAsync(CancellationToken cancellationToken)
    {
        var manager = _manager;
        if (manager is null)
        {
            _state.Update(MetadataSnapshot.Unknown("Unavailable"));
            return;
        }

        cancellationToken.ThrowIfCancellationRequested();

        var sessions = manager.GetSessions();
        LogSessionsIfChanged(sessions);

        var session = GetSelectedSession();
        if (session is null)
        {
            _state.Update(MetadataSnapshot.Unknown("NoSession"));
            return;
        }

        var playbackInfo = session.GetPlaybackInfo();
        var playbackStatus = playbackInfo.PlaybackStatus.ToString();
        var mediaProperties = await session.TryGetMediaPropertiesAsync();

        var snapshot = new MetadataSnapshot(
            Normalize(mediaProperties.Title),
            Normalize(mediaProperties.Artist),
            Normalize(mediaProperties.AlbumTitle),
            playbackStatus,
            Normalize(session.SourceAppUserModelId),
            DateTimeOffset.UtcNow);

        _state.Update(snapshot);
    }

    private GlobalSystemMediaTransportControlsSession? GetSelectedSession()
    {
        var manager = _manager;
        if (manager is null)
        {
            return null;
        }

        var sessions = manager.GetSessions();
        if (sessions.Count == 0)
        {
            return null;
        }

        var preferred = _config.Metadata.PreferredSourceAppUserModelId;
        if (_config.Metadata.SessionSelectionMode == MediaSessionSelectionMode.Manual &&
            !string.IsNullOrWhiteSpace(preferred))
        {
            var manual = sessions.FirstOrDefault(session =>
                session.SourceAppUserModelId.Equals(preferred, StringComparison.OrdinalIgnoreCase));

            if (manual is not null && GetStatus(manual) != GlobalSystemMediaTransportControlsSessionPlaybackStatus.Closed)
            {
                return manual;
            }

            var warning = $"Selected media session is unavailable or closed: {preferred}. Falling back to auto selection.";
            if (!warning.Equals(_lastManualFallbackWarning, StringComparison.Ordinal))
            {
                _lastManualFallbackWarning = warning;
                Log.Warn(warning);
            }
        }

        return sessions.FirstOrDefault(session =>
                   GetStatus(session) == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
            ?? manager.GetCurrentSession()
            ?? sessions.FirstOrDefault(session =>
                   GetStatus(session) != GlobalSystemMediaTransportControlsSessionPlaybackStatus.Closed)
            ?? sessions.FirstOrDefault();
    }

    private void LogSessionsIfChanged(IReadOnlyList<GlobalSystemMediaTransportControlsSession> sessions)
    {
        var selected = GetSelectedSession();
        var selectedSource = selected?.SourceAppUserModelId ?? "none";
        var signature = string.Join(
            "|",
            sessions.Select(session =>
            {
                var playbackStatus = "Unknown";
                try
                {
                    playbackStatus = session.GetPlaybackInfo().PlaybackStatus.ToString();
                }
                catch
                {
                    // Keep session logging best-effort.
                }

                return $"{session.SourceAppUserModelId}:{playbackStatus}";
            })) + $"|selected:{selectedSource}|mode:{_config.Metadata.SessionSelectionMode}|preferred:{_config.Metadata.PreferredSourceAppUserModelId}";

        if (signature == _lastSessionSignature)
        {
            return;
        }

        _lastSessionSignature = signature;

        if (sessions.Count == 0)
        {
            Log.Info("Windows media sessions: none.");
            return;
        }

        Log.Info($"Windows media sessions: selected={selectedSource} mode={_config.Metadata.SessionSelectionMode}");
        foreach (var session in sessions)
        {
            var playbackStatus = "Unknown";
            try
            {
                playbackStatus = session.GetPlaybackInfo().PlaybackStatus.ToString();
            }
            catch
            {
                // Keep session logging best-effort.
            }

            var marker = selected is not null && SameSource(session, selected) ? "*" : " ";
            Log.Info($" {marker} {session.SourceAppUserModelId} ({playbackStatus})");
        }
    }

    private async Task<MediaControlResult> TryControlAsync(
        string command,
        Func<GlobalSystemMediaTransportControlsSession, IAsyncOperation<bool>> control)
    {
        var session = GetSelectedSession();
        if (session is null)
        {
            return new MediaControlResult(false, false, command, "No active Windows media session.");
        }

        var target = Normalize(session.SourceAppUserModelId);
        var status = GetStatus(session).ToString();

        try
        {
            Log.Info($"Windows media control: command={command} target={target} status={status}");
            var sent = await control(session);
            Log.Info($"Windows media control result: command={command} target={target} sent={sent}");
            await RefreshSafeAsync(CancellationToken.None);
            return new MediaControlResult(true, sent, command, sent ? $"OK: {target}" : $"The selected session rejected the command: {target}");
        }
        catch (Exception ex)
        {
            Log.Warn($"Windows media control failed: command={command} target={target} error={ex.Message}");
            return new MediaControlResult(true, false, command, ex.Message);
        }
    }

    private static GlobalSystemMediaTransportControlsSessionPlaybackStatus GetStatus(
        GlobalSystemMediaTransportControlsSession session)
    {
        try
        {
            return session.GetPlaybackInfo().PlaybackStatus;
        }
        catch
        {
            return GlobalSystemMediaTransportControlsSessionPlaybackStatus.Closed;
        }
    }

    private static bool SameSource(
        GlobalSystemMediaTransportControlsSession first,
        GlobalSystemMediaTransportControlsSession second) =>
        first.SourceAppUserModelId.Equals(second.SourceAppUserModelId, StringComparison.OrdinalIgnoreCase);

    private static string Normalize(string? value)
    {
        return string.IsNullOrWhiteSpace(value) ? "Unknown" : value.Trim();
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();

        if (_pollingTask is not null)
        {
            try
            {
                await _pollingTask;
            }
            catch (OperationCanceledException)
            {
                // Expected during shutdown.
            }
        }

        _stop.Dispose();
    }
}

public sealed record MediaSessionInfo(
    string SourceAppUserModelId,
    string PlaybackStatus,
    string Title,
    string Artist,
    string Album,
    bool IsSelected);

public sealed record MediaSessionsResponse(
    string SelectionMode,
    string PreferredSourceAppUserModelId,
    string SelectedSourceAppUserModelId,
    IReadOnlyList<MediaSessionInfo> Sessions);
