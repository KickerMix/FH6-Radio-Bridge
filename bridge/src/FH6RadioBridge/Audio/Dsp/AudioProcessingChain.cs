using FH6RadioBridge.Config;
using FH6RadioBridge.IPC;

namespace FH6RadioBridge.Audio.Dsp;

public sealed class AudioProcessingChain
{
    private readonly object _gate = new();
    private readonly LoudnessNormalizer _normalizer = new(SharedAudioProtocol.SampleRate, SharedAudioProtocol.Channels);
    private readonly PeakingBiquad[] _bands;
    private DspSettings _settings;

    public AudioProcessingChain(DspConfig config)
    {
        _bands =
        [
            new PeakingBiquad(SharedAudioProtocol.SampleRate, 60.0f),
            new PeakingBiquad(SharedAudioProtocol.SampleRate, 250.0f),
            new PeakingBiquad(SharedAudioProtocol.SampleRate, 1000.0f),
            new PeakingBiquad(SharedAudioProtocol.SampleRate, 4000.0f),
            new PeakingBiquad(SharedAudioProtocol.SampleRate, 12000.0f)
        ];

        Update(config);
    }

    public DspSettings Settings
    {
        get
        {
            lock (_gate)
            {
                return _settings;
            }
        }
    }

    public void Update(DspConfig config)
    {
        var next = DspSettings.FromConfig(config);

        lock (_gate)
        {
            _settings = next;
            _bands[0].SetGainDb(next.Eq60Hz);
            _bands[1].SetGainDb(next.Eq250Hz);
            _bands[2].SetGainDb(next.Eq1kHz);
            _bands[3].SetGainDb(next.Eq4kHz);
            _bands[4].SetGainDb(next.Eq12kHz);
        }
    }

    public void Process(float[] samples, int sampleCount)
    {
        if (sampleCount <= 0)
        {
            return;
        }

        lock (_gate)
        {
            if (_settings.LoudnessNormalization)
            {
                _normalizer.Process(samples, sampleCount);
            }

            if (_settings.EqualizerEnabled)
            {
                foreach (var band in _bands)
                {
                    band.ProcessInterleavedStereo(samples, sampleCount);
                }
            }
        }
    }
}

public readonly record struct DspSettings(
    bool LoudnessNormalization,
    bool EqualizerEnabled,
    float Eq60Hz,
    float Eq250Hz,
    float Eq1kHz,
    float Eq4kHz,
    float Eq12kHz)
{
    public static DspSettings FromConfig(DspConfig config) => new(
        config.LoudnessNormalization,
        config.EqualizerEnabled,
        ClampGain(config.Eq60Hz),
        ClampGain(config.Eq250Hz),
        ClampGain(config.Eq1kHz),
        ClampGain(config.Eq4kHz),
        ClampGain(config.Eq12kHz));

    private static float ClampGain(float value) => Math.Clamp(value, -6.0f, 6.0f);
}
