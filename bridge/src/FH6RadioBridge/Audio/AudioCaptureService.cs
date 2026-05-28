using NAudio.CoreAudioApi;
using NAudio.Wave;
using FH6RadioBridge.IPC;
using FH6RadioBridge.Logging;

namespace FH6RadioBridge.Audio;

public sealed class AudioCaptureService : IDisposable
{
    private readonly IWaveIn _capture;
    private readonly AudioResampler _resampler;
    private readonly Action<float[], int> _samplesAvailable;
    private readonly object _gate = new();
    private readonly byte[] _outputBytes;
    private readonly float[] _outputSamples;
    private bool _disposed;

    private AudioCaptureService(IWaveIn capture, string deviceDisplayName, Action<float[], int> samplesAvailable)
    {
        _capture = capture;
        DeviceDisplayName = deviceDisplayName;
        _samplesAvailable = samplesAvailable;
        _resampler = new AudioResampler(capture.WaveFormat);
        _outputBytes = new byte[SharedAudioProtocol.SampleRate * SharedAudioProtocol.FrameBytes / 5];
        _outputSamples = new float[_outputBytes.Length / sizeof(float)];

        _capture.DataAvailable += OnDataAvailable;
        _capture.RecordingStopped += OnRecordingStopped;
    }

    public string DeviceDisplayName { get; }

    public static AudioCaptureService Create(AudioCaptureOptions options, Action<float[], int> samplesAvailable)
    {
        if (options.UseDefaultLoopback)
        {
            var capture = new WasapiLoopbackCapture();
            var name = GetDefaultRenderDeviceName();
            return new AudioCaptureService(capture, $"Default loopback: {name}", samplesAvailable);
        }

        var index = options.DeviceIndex ?? AudioDeviceSelector.FindCaptureDeviceIndex(options.DeviceName!);
        var caps = WaveInEvent.GetCapabilities(index);
        var waveIn = new WaveInEvent
        {
            DeviceNumber = index,
            WaveFormat = new WaveFormat(SharedAudioProtocol.SampleRate, 16, SharedAudioProtocol.Channels),
            BufferMilliseconds = 50,
            NumberOfBuffers = 3
        };

        return new AudioCaptureService(waveIn, $"[{index}] {caps.ProductName}", samplesAvailable);
    }

    public void Start()
    {
        ThrowIfDisposed();

        Log.Info($"Capture device: {DeviceDisplayName}");
        Log.Info($"Input format: {_capture.WaveFormat}");
        Log.Info($"Output format: {_resampler.OutputFormat}");

        _capture.StartRecording();
    }

    public void Stop()
    {
        if (_disposed)
        {
            return;
        }

        try
        {
            _capture.StopRecording();
        }
        catch (Exception ex)
        {
            Log.Warn($"Failed to stop capture cleanly: {ex.Message}");
        }
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        if (e.BytesRecorded <= 0)
        {
            return;
        }

        lock (_gate)
        {
            try
            {
                _resampler.AddSamples(e.Buffer, 0, e.BytesRecorded);

                while (true)
                {
                    var bytesRead = _resampler.Read(_outputBytes, 0, _outputBytes.Length);
                    if (bytesRead <= 0)
                    {
                        break;
                    }

                    bytesRead -= bytesRead % sizeof(float);
                    if (bytesRead <= 0)
                    {
                        break;
                    }

                    var sampleCount = bytesRead / sizeof(float);
                    Buffer.BlockCopy(_outputBytes, 0, _outputSamples, 0, bytesRead);
                    _samplesAvailable(_outputSamples, sampleCount);

                    if (bytesRead < _outputBytes.Length)
                    {
                        break;
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("Capture/resample error.", ex);
            }
        }
    }

    private static void OnRecordingStopped(object? sender, StoppedEventArgs e)
    {
        if (e.Exception is not null)
        {
            Log.Error("Capture stopped because of an audio backend error.", e.Exception);
        }
        else
        {
            Log.Info("Capture stopped.");
        }
    }

    private static string GetDefaultRenderDeviceName()
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            using var device = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            return device.FriendlyName;
        }
        catch
        {
            return "unknown render device";
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _capture.DataAvailable -= OnDataAvailable;
        _capture.RecordingStopped -= OnRecordingStopped;
        _resampler.Dispose();
        _capture.Dispose();
    }
}
