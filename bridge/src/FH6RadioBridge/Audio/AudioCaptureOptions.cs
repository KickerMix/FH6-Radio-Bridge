namespace FH6RadioBridge.Audio;

public sealed record AudioCaptureOptions(bool UseDefaultLoopback, int? DeviceIndex, string? DeviceName)
{
    public static AudioCaptureOptions DefaultLoopback() => new(true, null, null);

    public static AudioCaptureOptions CaptureDeviceIndex(int index) => new(false, index, null);

    public static AudioCaptureOptions CaptureDeviceName(string name) => new(false, null, name);
}
