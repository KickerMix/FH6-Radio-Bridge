using Windows.Foundation;
using Windows.Media.Control;
using FH6RadioBridge.Logging;

namespace FH6RadioBridge.Metadata;

public sealed class MediaSessionMetadataService : IAsyncDisposable
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);
    private readonly MetadataState _state;
    private readonly CancellationTokenSource _stop = new();
    private GlobalSystemMediaTransportControlsSessionManager? _manager;
    private Task? _pollingTask;
    private string _lastSessionSignature = string.Empty;

    public MediaSessionMetadataService(MetadataState state)
    {
        _state = state;
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

        var session = manager.GetCurrentSession() ?? sessions.FirstOrDefault();
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

    private void LogSessionsIfChanged(IReadOnlyList<GlobalSystemMediaTransportControlsSession> sessions)
    {
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
            }));

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

        Log.Info("Windows media sessions:");
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

            Log.Info($"  {session.SourceAppUserModelId} ({playbackStatus})");
        }
    }

    private async Task<MediaControlResult> TryControlAsync(
        string command,
        Func<GlobalSystemMediaTransportControlsSession, IAsyncOperation<bool>> control)
    {
        var session = _manager?.GetCurrentSession();
        if (session is null)
        {
            return new MediaControlResult(false, false, command, "No active Windows media session.");
        }

        try
        {
            var sent = await control(session);
            await RefreshSafeAsync(CancellationToken.None);
            return new MediaControlResult(true, sent, command, sent ? "OK" : "The current session rejected the command.");
        }
        catch (Exception ex)
        {
            return new MediaControlResult(true, false, command, ex.Message);
        }
    }

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
