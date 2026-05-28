namespace FH6RadioBridge.Audio;

public readonly record struct AudioLevels(float PeakL, float PeakR, float RmsL, float RmsR);

public static class AudioLevelMeter
{
    public static AudioLevels Calculate(float[] samples, int offsetSamples, int sampleCount, int channels)
    {
        if (channels != 2 || sampleCount < 2)
        {
            return new AudioLevels(0, 0, 0, 0);
        }

        var frames = sampleCount / channels;
        double sumL = 0;
        double sumR = 0;
        var peakL = 0f;
        var peakR = 0f;

        for (var i = 0; i < frames; i++)
        {
            var left = samples[offsetSamples + (i * channels)];
            var right = samples[offsetSamples + (i * channels) + 1];
            var absL = Math.Abs(left);
            var absR = Math.Abs(right);

            if (absL > peakL)
            {
                peakL = absL;
            }

            if (absR > peakR)
            {
                peakR = absR;
            }

            sumL += left * left;
            sumR += right * right;
        }

        var rmsL = frames > 0 ? (float)Math.Sqrt(sumL / frames) : 0f;
        var rmsR = frames > 0 ? (float)Math.Sqrt(sumR / frames) : 0f;
        return new AudioLevels(peakL, peakR, rmsL, rmsR);
    }
}
