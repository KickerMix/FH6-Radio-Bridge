using NAudio.Wave;

namespace FH6RadioBridge.Audio;

public static class AudioDeviceSelector
{
    public static void PrintCaptureDevices()
    {
        Console.WriteLine("Capture devices:");

        for (var i = 0; i < WaveInEvent.DeviceCount; i++)
        {
            var caps = WaveInEvent.GetCapabilities(i);
            Console.WriteLine($"  [{i}] {caps.ProductName} ({caps.Channels} channel(s))");
        }

        Console.WriteLine();
        Console.WriteLine("Other capture mode:");
        Console.WriteLine("  --loopback-default    Capture the default Windows playback device via WASAPI loopback.");
    }

    public static int FindCaptureDeviceIndex(string deviceName)
    {
        if (string.IsNullOrWhiteSpace(deviceName))
        {
            throw new ArgumentException("Device name is empty.", nameof(deviceName));
        }

        for (var i = 0; i < WaveInEvent.DeviceCount; i++)
        {
            var caps = WaveInEvent.GetCapabilities(i);
            if (string.Equals(caps.ProductName, deviceName, StringComparison.OrdinalIgnoreCase) ||
                caps.ProductName.Contains(deviceName, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }

        throw new InvalidOperationException($"Capture device was not found: {deviceName}");
    }
}
