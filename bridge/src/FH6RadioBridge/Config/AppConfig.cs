using System.Text.Json;
using System.Text.Json.Serialization;
using FH6RadioBridge.IPC;
using FH6RadioBridge.Logging;

namespace FH6RadioBridge.Config;

public sealed class AppConfig
{
    public const int CurrentConfigVersion = 3;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) }
    };

    [JsonIgnore]
    public string ConfigPath { get; private set; } = string.Empty;

    public int ConfigVersion { get; set; } = CurrentConfigVersion;

    public AudioConfig Audio { get; set; } = new();

    public IpcConfig Ipc { get; set; } = new();

    public ApiConfig Api { get; set; } = new();

    public MetadataConfig Metadata { get; set; } = new();

    public HookConfig Hook { get; set; } = new();

    public PlaybackAutomationConfig PlaybackAutomation { get; set; } = new();

    public DspConfig Dsp { get; set; } = new();

    public static AppConfig LoadOrCreate()
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "FH6RadioBridge");
        var path = Path.Combine(directory, "config.json");

        try
        {
            Directory.CreateDirectory(directory);

            if (File.Exists(path))
            {
                var json = File.ReadAllText(path);
                var loaded = JsonSerializer.Deserialize<AppConfig>(json, JsonOptions) ?? new AppConfig();
                loaded.ConfigPath = path;
                if (!json.Contains("\"configVersion\"", StringComparison.OrdinalIgnoreCase))
                {
                    loaded.ConfigVersion = 0;
                }

                loaded.EnsureDefaults();

                if (loaded.ApplyMigrations())
                {
                    loaded.Save();
                }

                return loaded;
            }

            var config = new AppConfig { ConfigPath = path };
            config.Save();
            return config;
        }
        catch (Exception ex)
        {
            Log.Warn($"Could not load or create config at {path}: {ex.Message}");
            var fallback = new AppConfig { ConfigPath = path };
            fallback.EnsureDefaults();
            return fallback;
        }
    }

    public void Save()
    {
        if (string.IsNullOrWhiteSpace(ConfigPath))
        {
            return;
        }

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath)!);
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(this, JsonOptions));
        }
        catch (Exception ex)
        {
            Log.Warn($"Could not save config at {ConfigPath}: {ex.Message}");
        }
    }

    private void EnsureDefaults()
    {
        Audio ??= new AudioConfig();
        Ipc ??= new IpcConfig();
        Api ??= new ApiConfig();
        Metadata ??= new MetadataConfig();
        Hook ??= new HookConfig();
        PlaybackAutomation ??= new PlaybackAutomationConfig();
        Dsp ??= new DspConfig();
    }

    private bool ApplyMigrations()
    {
        var changed = false;

        if (ConfigVersion < 2)
        {
            Metadata.UseWindowsMediaSession = true;
            ConfigVersion = 2;
            changed = true;
        }

        if (ConfigVersion < 3)
        {
            if (Api.Port == 47860)
            {
                Api.Port = 8420;
            }

            PlaybackAutomation ??= new PlaybackAutomationConfig();
            Dsp ??= new DspConfig();
            ConfigVersion = CurrentConfigVersion;
            changed = true;
        }

        if (ConfigVersion != CurrentConfigVersion)
        {
            ConfigVersion = CurrentConfigVersion;
            changed = true;
        }

        return changed;
    }
}

public sealed class AudioConfig
{
    public string CaptureDeviceName { get; set; } = string.Empty;

    public int CaptureDeviceIndex { get; set; } = -1;

    public int SampleRate { get; set; } = SharedAudioProtocol.SampleRate;

    public int Channels { get; set; } = SharedAudioProtocol.Channels;

    public float Volume { get; set; } = 1.0f;

    public bool UseLimiter { get; set; } = true;
}

public sealed class IpcConfig
{
    public string SharedMemoryName { get; set; } = SharedAudioProtocol.GlobalMemoryName;

    public string EventName { get; set; } = SharedAudioProtocol.GlobalEventName;

    public int BufferSeconds { get; set; } = SharedAudioProtocol.BufferSeconds;
}

public sealed class ApiConfig
{
    public string Host { get; set; } = "127.0.0.1";

    public int Port { get; set; } = 8420;
}

public sealed class MetadataConfig
{
    public bool UseWindowsMediaSession { get; set; } = true;
}

public sealed class HookConfig
{
    public bool EnableFmodHook { get; set; } = false;

    public bool PlayOnlyWhenR10Active { get; set; } = true;
}

public sealed class PlaybackAutomationConfig
{
    public RaceStartAction RaceStartAction { get; set; } = RaceStartAction.None;

    public bool QuickStationSkip { get; set; } = false;
}

public enum RaceStartAction
{
    None,
    NextTrack,
    RestartCurrent
}

public sealed class DspConfig
{
    public bool LoudnessNormalization { get; set; } = false;

    public bool EqualizerEnabled { get; set; } = false;

    public float Eq60Hz { get; set; } = 0.0f;

    public float Eq250Hz { get; set; } = 0.0f;

    public float Eq1kHz { get; set; } = 0.0f;

    public float Eq4kHz { get; set; } = 0.0f;

    public float Eq12kHz { get; set; } = 0.0f;
}
