using System.Diagnostics;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using FH6RadioBridge.Audio.Dsp;
using FH6RadioBridge.Config;
using FH6RadioBridge.IPC;
using FH6RadioBridge.Logging;
using FH6RadioBridge.Metadata;

namespace FH6RadioBridge.Web;

public sealed class LocalApiServer : IAsyncDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) }
    };

    private readonly WebApplication _app;
    private readonly AppConfig _config;
    private readonly SharedAudioWriter _writer;
    private readonly AudioProcessingChain _dsp;
    private readonly MetadataState _metadata;
    private readonly MediaSessionMetadataService _metadataService;
    private readonly string _captureDeviceName;

    public LocalApiServer(
        AppConfig config,
        SharedAudioWriter writer,
        AudioProcessingChain dsp,
        MetadataState metadata,
        MediaSessionMetadataService metadataService,
        string captureDeviceName)
    {
        _config = config;
        _writer = writer;
        _dsp = dsp;
        _metadata = metadata;
        _metadataService = metadataService;
        _captureDeviceName = captureDeviceName;

        var builder = WebApplication.CreateBuilder(new WebApplicationOptions
        {
            Args = []
        });
        builder.Logging.ClearProviders();
        builder.Services.ConfigureHttpJsonOptions(options =>
        {
            options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.CamelCase;
            options.SerializerOptions.Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping;
            options.SerializerOptions.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        });
        builder.WebHost.UseUrls($"http://{config.Api.Host}:{config.Api.Port}");

        _app = builder.Build();
        MapEndpoints(_app);
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        await _app.StartAsync(cancellationToken);
        Log.Info($"Local dashboard/API listening at http://{_config.Api.Host}:{_config.Api.Port}/");
    }

    public Task StopAsync(CancellationToken cancellationToken = default) => _app.StopAsync(cancellationToken);

    private void MapEndpoints(WebApplication app)
    {
        app.UseDefaultFiles();
        app.UseStaticFiles();

        app.MapGet("/api/state", GetState);
        app.MapGet("/api/config", GetConfig);
        app.MapPut("/api/config", async (HttpContext context) => await UpdateConfigAsync(context));
        app.MapGet("/api/metadata", () => Json(_metadata.Current));
        app.MapPost("/api/settings/volume", async (HttpContext context) => await SetVolumeAsync(context));
        app.MapPost("/api/control/playpause", async () => Json(await _metadataService.TryPlayPauseAsync()));
        app.MapPost("/api/control/next", async () => Json(await _metadataService.TryNextAsync()));
        app.MapPost("/api/control/previous", async () => Json(await _metadataService.TryPreviousAsync()));
        app.MapPost("/api/control/restart", async () => Json(await _metadataService.TryRestartCurrentAsync()));
        app.MapPost("/api/hook/event", async (HttpContext context) => await HandleHookEventAsync(context));
    }

    private static IResult Json(object value) => Results.Json(value, JsonOptions);

    private IResult GetState()
    {
        var snapshot = _writer.GetSnapshot();
        var fillFrames = snapshot.ReadFrame > 0 && snapshot.WriteFrame >= snapshot.ReadFrame
            ? Math.Min(snapshot.WriteFrame - snapshot.ReadFrame, (ulong)SharedAudioProtocol.BufferFrames)
            : 0UL;

        return Json(new
        {
            bridgeRunning = true,
            sourceAvailable = (snapshot.Flags & (uint)AudioSharedFlags.SourceAvailable) != 0,
            captureDevice = _captureDeviceName,
            sharedMemoryName = snapshot.SharedMemoryName,
            headerValid = snapshot.HeaderValid,
            sampleRate = SharedAudioProtocol.SampleRate,
            channels = SharedAudioProtocol.Channels,
            bufferFrames = SharedAudioProtocol.BufferFrames,
            writeFrame = snapshot.WriteFrame,
            readFrame = snapshot.ReadFrame,
            writerAgeMs = GetWriterAgeMilliseconds(snapshot.LastWriteQpc),
            bufferFillApproxMs = fillFrames * 1000 / SharedAudioProtocol.SampleRate,
            peakL = snapshot.PeakL,
            peakR = snapshot.PeakR,
            rmsL = snapshot.RmsL,
            rmsR = snapshot.RmsR,
            volume = snapshot.Volume,
            underrunCount = snapshot.UnderrunCount,
            metadata = _metadata.Current,
            playbackAutomation = _config.PlaybackAutomation,
            dsp = _dsp.Settings
        });
    }

    private IResult GetConfig()
    {
        return Json(new
        {
            _config.ConfigVersion,
            _config.Api,
            _config.Audio,
            _config.Metadata,
            _config.Hook,
            _config.PlaybackAutomation,
            _config.Dsp
        });
    }

    private async Task<IResult> UpdateConfigAsync(HttpContext context)
    {
        try
        {
            var request = await JsonSerializer.DeserializeAsync<ConfigUpdateRequest>(
                context.Request.Body,
                JsonOptions,
                context.RequestAborted);

            if (request is null)
            {
                return Results.BadRequest(new { error = "Expected JSON config update body." });
            }

            if (request.Audio is not null)
            {
                if (request.Audio.Volume is float volume)
                {
                    var clamped = Math.Clamp(volume, 0.0f, 2.0f);
                    _config.Audio.Volume = clamped;
                    _writer.SetVolume(clamped);
                }

                if (request.Audio.UseLimiter is bool useLimiter)
                {
                    _config.Audio.UseLimiter = useLimiter;
                    Log.Warn("useLimiter changes are saved, but the current writer applies them after Bridge restart.");
                }
            }

            if (request.PlaybackAutomation is not null)
            {
                if (request.PlaybackAutomation.RaceStartAction is RaceStartAction raceStartAction)
                {
                    _config.PlaybackAutomation.RaceStartAction = raceStartAction;
                }

                if (request.PlaybackAutomation.QuickStationSkip is bool quickStationSkip)
                {
                    _config.PlaybackAutomation.QuickStationSkip = quickStationSkip;
                }
            }

            if (request.Dsp is not null)
            {
                if (request.Dsp.LoudnessNormalization is bool loudnessNormalization)
                {
                    _config.Dsp.LoudnessNormalization = loudnessNormalization;
                }

                if (request.Dsp.EqualizerEnabled is bool equalizerEnabled)
                {
                    _config.Dsp.EqualizerEnabled = equalizerEnabled;
                }

                if (request.Dsp.Eq60Hz is float eq60Hz)
                {
                    _config.Dsp.Eq60Hz = Math.Clamp(eq60Hz, -6.0f, 6.0f);
                }

                if (request.Dsp.Eq250Hz is float eq250Hz)
                {
                    _config.Dsp.Eq250Hz = Math.Clamp(eq250Hz, -6.0f, 6.0f);
                }

                if (request.Dsp.Eq1kHz is float eq1kHz)
                {
                    _config.Dsp.Eq1kHz = Math.Clamp(eq1kHz, -6.0f, 6.0f);
                }

                if (request.Dsp.Eq4kHz is float eq4kHz)
                {
                    _config.Dsp.Eq4kHz = Math.Clamp(eq4kHz, -6.0f, 6.0f);
                }

                if (request.Dsp.Eq12kHz is float eq12kHz)
                {
                    _config.Dsp.Eq12kHz = Math.Clamp(eq12kHz, -6.0f, 6.0f);
                }

                _dsp.Update(_config.Dsp);
            }

            _config.Save();
            return GetConfig();
        }
        catch (JsonException)
        {
            return Results.BadRequest(new { error = "Invalid JSON." });
        }
    }

    private async Task<IResult> SetVolumeAsync(HttpContext context)
    {
        try
        {
            var request = await JsonSerializer.DeserializeAsync<VolumeRequest>(
                context.Request.Body,
                JsonOptions,
                context.RequestAborted);

            if (request is null)
            {
                return Results.BadRequest(new { error = "Expected JSON body with a volume property." });
            }

            var volume = Math.Clamp(request.Volume, 0.0f, 2.0f);
            _config.Audio.Volume = volume;
            _writer.SetVolume(volume);
            _config.Save();
            return Json(new { volume });
        }
        catch (JsonException)
        {
            return Results.BadRequest(new { error = "Invalid JSON." });
        }
    }

    private async Task<IResult> HandleHookEventAsync(HttpContext context)
    {
        try
        {
            var request = await JsonSerializer.DeserializeAsync<HookEventRequest>(
                context.Request.Body,
                JsonOptions,
                context.RequestAborted);

            if (request is null || string.IsNullOrWhiteSpace(request.Type))
            {
                return Results.BadRequest(new { error = "Expected JSON body with a type property." });
            }

            Log.Info($"Hook event received: {request.Type}");

            if (request.Type.Equals("raceStart", StringComparison.OrdinalIgnoreCase))
            {
                return Json(await HandleRaceStartActionAsync());
            }

            if (request.Type.Equals("raceRestart", StringComparison.OrdinalIgnoreCase))
            {
                return Json(new { handled = false, reason = "raceRestart is ignored by Race Start Action." });
            }

            if (request.Type.Equals("quickStationSkip", StringComparison.OrdinalIgnoreCase))
            {
                if (!_config.PlaybackAutomation.QuickStationSkip)
                {
                    return Json(new { handled = false, reason = "Quick station skip is disabled." });
                }

                var result = await _metadataService.TryNextAsync();
                return Json(new { handled = result.Sent, control = result });
            }

            return Json(new { handled = false, reason = "Unknown event type." });
        }
        catch (JsonException)
        {
            return Results.BadRequest(new { error = "Invalid JSON." });
        }
    }

    private async Task<object> HandleRaceStartActionAsync()
    {
        switch (_config.PlaybackAutomation.RaceStartAction)
        {
            case RaceStartAction.NextTrack:
            {
                var result = await _metadataService.TryNextAsync();
                return new { handled = result.Sent, action = "nextTrack", control = result };
            }

            case RaceStartAction.RestartCurrent:
            {
                var result = await _metadataService.TryRestartCurrentAsync();
                return new { handled = result.Sent, action = "restartCurrent", control = result };
            }

            default:
                return new { handled = false, action = "none", reason = "Race start action is disabled." };
        }
    }

    private static long GetWriterAgeMilliseconds(ulong lastWriteQpc)
    {
        if (lastWriteQpc == 0)
        {
            return -1;
        }

        var delta = Stopwatch.GetTimestamp() - (long)lastWriteQpc;
        return delta < 0 ? 0 : delta * 1000 / Stopwatch.Frequency;
    }

    public async ValueTask DisposeAsync()
    {
        await _app.DisposeAsync();
    }

    private sealed class VolumeRequest
    {
        public float Volume { get; set; } = 1.0f;
    }

    private sealed class HookEventRequest
    {
        public string Type { get; set; } = string.Empty;
    }

    private sealed class ConfigUpdateRequest
    {
        public AudioUpdateRequest? Audio { get; set; }
        public PlaybackAutomationUpdateRequest? PlaybackAutomation { get; set; }
        public DspUpdateRequest? Dsp { get; set; }
    }

    private sealed class AudioUpdateRequest
    {
        public float? Volume { get; set; }
        public bool? UseLimiter { get; set; }
    }

    private sealed class PlaybackAutomationUpdateRequest
    {
        public RaceStartAction? RaceStartAction { get; set; }
        public bool? QuickStationSkip { get; set; }
    }

    private sealed class DspUpdateRequest
    {
        public bool? LoudnessNormalization { get; set; }
        public bool? EqualizerEnabled { get; set; }
        public float? Eq60Hz { get; set; }
        public float? Eq250Hz { get; set; }
        public float? Eq1kHz { get; set; }
        public float? Eq4kHz { get; set; }
        public float? Eq12kHz { get; set; }
    }
}
