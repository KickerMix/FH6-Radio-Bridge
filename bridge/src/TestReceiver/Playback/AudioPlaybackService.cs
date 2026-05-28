using NAudio.CoreAudioApi;
using NAudio.Wave;
using TestReceiver.IPC;

namespace TestReceiver.Playback;

public sealed class AudioPlaybackService : IWaveProvider, IDisposable
{
    private readonly SharedAudioReader _reader;
    private IWavePlayer? _output;

    public AudioPlaybackService(SharedAudioReader reader)
    {
        _reader = reader;
        WaveFormat = WaveFormat.CreateIeeeFloatWaveFormat(SharedAudioProtocol.SampleRate, SharedAudioProtocol.Channels);
    }

    public WaveFormat WaveFormat { get; }

    public void Start()
    {
        _output = CreateOutput();
        _output.Init(this);
        _output.Play();
    }

    public int Read(byte[] buffer, int offset, int count) => _reader.Read(buffer, offset, count);

    private static IWavePlayer CreateOutput()
    {
        try
        {
            return new WasapiOut(AudioClientShareMode.Shared, false, 100);
        }
        catch
        {
            return new WaveOutEvent { DesiredLatency = 120 };
        }
    }

    public void Dispose()
    {
        _output?.Stop();
        _output?.Dispose();
    }
}
