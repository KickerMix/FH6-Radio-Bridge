namespace FH6RadioBridge.Audio.Dsp;

public sealed class LoudnessNormalizer
{
    private readonly int _channels;
    private readonly float _attackCoefficient;
    private readonly float _releaseCoefficient;
    private float _smoothedGain = 1.0f;

    private const float TargetRmsDb = -18.0f;
    private const float MinGain = 0.25f;
    private const float MaxGain = 4.0f;
    private const float SilenceThreshold = 0.00001f;

    public LoudnessNormalizer(int sampleRate, int channels)
    {
        _channels = Math.Max(1, channels);
        _attackCoefficient = Coefficient(sampleRate, 0.08f);
        _releaseCoefficient = Coefficient(sampleRate, 1.6f);
    }

    public void Process(float[] samples, int sampleCount)
    {
        var frames = sampleCount / _channels;
        if (frames <= 0)
        {
            return;
        }

        double sumSquares = 0;
        for (var i = 0; i < sampleCount; i++)
        {
            var sample = samples[i];
            sumSquares += sample * sample;
        }

        var rms = Math.Sqrt(sumSquares / sampleCount);
        if (rms < SilenceThreshold)
        {
            return;
        }

        var targetRms = DbToLinear(TargetRmsDb);
        var desiredGain = (float)Math.Clamp(targetRms / rms, MinGain, MaxGain);
        var coefficient = desiredGain < _smoothedGain ? _attackCoefficient : _releaseCoefficient;

        for (var frame = 0; frame < frames; frame++)
        {
            _smoothedGain += (desiredGain - _smoothedGain) * coefficient;
            var baseIndex = frame * _channels;
            for (var ch = 0; ch < _channels; ch++)
            {
                samples[baseIndex + ch] *= _smoothedGain;
            }
        }
    }

    private static float DbToLinear(float db) => MathF.Pow(10.0f, db / 20.0f);

    private static float Coefficient(int sampleRate, float timeSeconds)
    {
        var frames = Math.Max(1.0f, sampleRate * timeSeconds);
        return 1.0f - MathF.Exp(-1.0f / frames);
    }
}
