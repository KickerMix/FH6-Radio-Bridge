namespace FH6RadioBridge.Audio.Dsp;

public sealed class PeakingBiquad
{
    private readonly float _sampleRate;
    private readonly float _frequency;
    private readonly float _q;
    private BiquadCoefficients _coefficients;
    private BiquadState _left;
    private BiquadState _right;

    public PeakingBiquad(float sampleRate, float frequency, float q = 1.0f)
    {
        _sampleRate = sampleRate;
        _frequency = frequency;
        _q = q;
        SetGainDb(0.0f);
    }

    public void SetGainDb(float gainDb)
    {
        gainDb = Math.Clamp(gainDb, -6.0f, 6.0f);

        if (Math.Abs(gainDb) < 0.001f)
        {
            _coefficients = BiquadCoefficients.Identity;
            return;
        }

        var a = MathF.Pow(10.0f, gainDb / 40.0f);
        var w0 = 2.0f * MathF.PI * _frequency / _sampleRate;
        var cosW0 = MathF.Cos(w0);
        var sinW0 = MathF.Sin(w0);
        var alpha = sinW0 / (2.0f * _q);

        var b0 = 1.0f + alpha * a;
        var b1 = -2.0f * cosW0;
        var b2 = 1.0f - alpha * a;
        var a0 = 1.0f + alpha / a;
        var a1 = -2.0f * cosW0;
        var a2 = 1.0f - alpha / a;

        _coefficients = new BiquadCoefficients(
            b0 / a0,
            b1 / a0,
            b2 / a0,
            a1 / a0,
            a2 / a0);
    }

    public void ProcessInterleavedStereo(float[] samples, int sampleCount)
    {
        if (_coefficients.IsIdentity)
        {
            return;
        }

        for (var i = 0; i + 1 < sampleCount; i += 2)
        {
            samples[i] = ProcessSample(samples[i], ref _left);
            samples[i + 1] = ProcessSample(samples[i + 1], ref _right);
        }
    }

    private float ProcessSample(float input, ref BiquadState state)
    {
        var c = _coefficients;
        var output = c.B0 * input + state.Z1;
        state.Z1 = c.B1 * input - c.A1 * output + state.Z2;
        state.Z2 = c.B2 * input - c.A2 * output;
        return output;
    }

    private readonly record struct BiquadCoefficients(float B0, float B1, float B2, float A1, float A2)
    {
        public static readonly BiquadCoefficients Identity = new(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);

        public bool IsIdentity => B0 == 1.0f && B1 == 0.0f && B2 == 0.0f && A1 == 0.0f && A2 == 0.0f;
    }

    private struct BiquadState
    {
        public float Z1;
        public float Z2;
    }
}
