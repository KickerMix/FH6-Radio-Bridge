using FH6RadioBridge.Audio;
using FH6RadioBridge.Audio.Dsp;
using FH6RadioBridge.Config;
using FH6RadioBridge.IPC;
using FH6RadioBridge.Logging;
using FH6RadioBridge.Metadata;
using FH6RadioBridge.Web;

var cli = CommandLineOptions.Parse(args);

if (cli.ShowHelp)
{
    CommandLineOptions.PrintUsage();
    return 0;
}

if (cli.ListDevices)
{
    AudioDeviceSelector.PrintCaptureDevices();
    return 0;
}

var config = AppConfig.LoadOrCreate();
var captureOptions = cli.ToCaptureOptions(config);

if (captureOptions is null)
{
    CommandLineOptions.PrintUsage();
    return 2;
}

using var cts = new CancellationTokenSource();
var stopped = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);

Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cts.Cancel();
    stopped.TrySetResult(null);
};

try
{
    Log.Info("FH6 Radio Bridge starting.");
    Log.Info($"Config path: {config.ConfigPath}");

    using var writer = SharedAudioWriter.Create(config.Audio.Volume, config.Audio.UseLimiter);
    var dsp = new AudioProcessingChain(config.Dsp);
    using var capture = AudioCaptureService.Create(captureOptions, (samples, sampleCount) =>
    {
        dsp.Process(samples, sampleCount);
        writer.WriteSamples(samples, sampleCount);
    });
    var metadata = MetadataState.Unknown();
    await using var metadataService = new MediaSessionMetadataService(metadata);
    if (config.Metadata.UseWindowsMediaSession)
    {
        await metadataService.StartAsync(cts.Token);
    }
    else
    {
        Log.Warn("Windows media session metadata is disabled in config.");
    }

    await using var apiServer = new LocalApiServer(config, writer, dsp, metadata, metadataService, capture.DeviceDisplayName);

    await apiServer.StartAsync(cts.Token);
    capture.Start();

    Log.Info("Bridge is running. Press Ctrl+C to stop.");
    await stopped.Task.WaitAsync(cts.Token);
}
catch (OperationCanceledException)
{
    // Expected during Ctrl+C shutdown.
}
catch (Exception ex)
{
    Log.Error("Bridge failed.", ex);
    return 1;
}
finally
{
    Log.Info("FH6 Radio Bridge stopped.");
}

return 0;

internal sealed class CommandLineOptions
{
    public bool ShowHelp { get; private set; }
    public bool ListDevices { get; private set; }
    public bool LoopbackDefault { get; private set; }
    public int? DeviceIndex { get; private set; }
    public string? DeviceName { get; private set; }

    public static CommandLineOptions Parse(string[] args)
    {
        var options = new CommandLineOptions();

        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];

            if (arg is "--help" or "-h" or "/?")
            {
                options.ShowHelp = true;
            }
            else if (arg == "--list-devices")
            {
                options.ListDevices = true;
            }
            else if (arg == "--loopback-default")
            {
                options.LoopbackDefault = true;
            }
            else if (arg == "--device-index" && i + 1 < args.Length && int.TryParse(args[++i], out var index))
            {
                options.DeviceIndex = index;
            }
            else if (arg.StartsWith("--device-index=", StringComparison.OrdinalIgnoreCase) &&
                     int.TryParse(arg["--device-index=".Length..], out var inlineIndex))
            {
                options.DeviceIndex = inlineIndex;
            }
            else if (arg == "--device" && i + 1 < args.Length)
            {
                options.DeviceName = args[++i];
            }
            else if (arg.StartsWith("--device=", StringComparison.OrdinalIgnoreCase))
            {
                options.DeviceName = arg["--device=".Length..];
            }
            else
            {
                Log.Warn($"Unknown argument: {arg}");
            }
        }

        return options;
    }

    public AudioCaptureOptions? ToCaptureOptions(AppConfig config)
    {
        if (LoopbackDefault)
        {
            return AudioCaptureOptions.DefaultLoopback();
        }

        if (DeviceIndex is int cliIndex)
        {
            return AudioCaptureOptions.CaptureDeviceIndex(cliIndex);
        }

        if (!string.IsNullOrWhiteSpace(DeviceName))
        {
            return AudioCaptureOptions.CaptureDeviceName(DeviceName);
        }

        if (config.Audio.CaptureDeviceIndex >= 0)
        {
            return AudioCaptureOptions.CaptureDeviceIndex(config.Audio.CaptureDeviceIndex);
        }

        if (!string.IsNullOrWhiteSpace(config.Audio.CaptureDeviceName))
        {
            return AudioCaptureOptions.CaptureDeviceName(config.Audio.CaptureDeviceName);
        }

        return null;
    }

    public static void PrintUsage()
    {
        Console.WriteLine("FH6 Radio Bridge");
        Console.WriteLine();
        Console.WriteLine("Usage:");
        Console.WriteLine("  FH6RadioBridge.exe --list-devices");
        Console.WriteLine("  FH6RadioBridge.exe --device-index <index>");
        Console.WriteLine("  FH6RadioBridge.exe --device \"<capture device name>\"");
        Console.WriteLine("  FH6RadioBridge.exe --loopback-default");
        Console.WriteLine();
        Console.WriteLine("The bridge captures already-playing system audio or a selected input device.");
        Console.WriteLine("It does not download, decrypt, or bypass DRM-protected audio.");
    }
}
