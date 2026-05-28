using NAudio.Wave;

namespace FH6RadioBridge.Audio;

public sealed class AudioResampler : IDisposable
{
    private readonly BufferedWaveProvider _sourceBuffer;
    private readonly MediaFoundationResampler _resampler;

    public AudioResampler(WaveFormat inputFormat)
    {
        InputFormat = inputFormat;
        OutputFormat = WaveFormat.CreateIeeeFloatWaveFormat(48000, 2);
        _sourceBuffer = new BufferedWaveProvider(inputFormat)
        {
            BufferDuration = TimeSpan.FromSeconds(2),
            DiscardOnBufferOverflow = true,
            ReadFully = false
        };
        _resampler = new MediaFoundationResampler(_sourceBuffer, OutputFormat)
        {
            ResamplerQuality = 60
        };
    }

    public WaveFormat InputFormat { get; }

    public WaveFormat OutputFormat { get; }

    public void AddSamples(byte[] buffer, int offset, int count) => _sourceBuffer.AddSamples(buffer, offset, count);

    public int Read(byte[] buffer, int offset, int count) => _resampler.Read(buffer, offset, count);

    public void Dispose() => _resampler.Dispose();
}
