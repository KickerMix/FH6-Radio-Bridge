using TestReceiver.IPC;
using TestReceiver.Playback;

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cts.Cancel();
};

using var reader = new SharedAudioReader();
using var playback = new AudioPlaybackService(reader);

Console.WriteLine("TestReceiver starting.");
Console.WriteLine("It will output silence until FH6 Radio Bridge shared memory is available.");
Console.WriteLine("Press Ctrl+C to stop.");

try
{
    playback.Start();

    while (!cts.IsCancellationRequested)
    {
        var state = reader.GetDebugState();
        Console.WriteLine(
            $"{DateTimeOffset.Now:HH:mm:ss} " +
            $"connected={state.Connected} " +
            $"map={state.SharedMemoryName ?? "-"} " +
            $"writeFrame={state.WriteFrame} " +
            $"readFrame={state.ReadFrame} " +
            $"latencyMs={state.LatencyMilliseconds} " +
            $"underruns={state.LocalUnderrunCount} " +
            $"peak=({state.PeakL:0.000},{state.PeakR:0.000}) " +
            $"{state.LastError}");

        await Task.Delay(TimeSpan.FromSeconds(1), cts.Token);
    }
}
catch (OperationCanceledException)
{
    // Expected during Ctrl+C shutdown.
}
