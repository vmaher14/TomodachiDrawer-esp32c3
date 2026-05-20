using System.Diagnostics;

namespace TomodachiDrawer.UI.Avalonia;

internal static class ESP32Flasher
{
    private const uint FlashOffset = 0x110000; // storage partition offset from partitions.csv

    // Find the ESP32's serial port when in normal (not download) mode.
    // In download mode esptool handles detection itself.
    public static string? FindESP32Port()
    {
        // ESP32-C3 DevKit shows up as one of these USB-serial chips
        var likelyPrefixes = new[] { "COM", "/dev/ttyUSB", "/dev/ttyACM", "/dev/cu.usbserial", "/dev/cu.usbmodem" };

        foreach (var portName in System.IO.Ports.SerialPort.GetPortNames())
        {
            if (likelyPrefixes.Any(p => portName.StartsWith(p, StringComparison.OrdinalIgnoreCase)))
                return portName;
        }
        return null;
    }

    // Flash a .tdld file to the ESP32 storage partition using esptool.
    // Returns (success, output log).
    public static async Task<(bool success, string log)> FlashTDLDAsync(
        string tdldPath,
        string? port = null,
        IProgress<string>? progress = null)
    {
        var args = BuildEsptoolArgs("write_flash", port, $"0x{FlashOffset:X} \"{tdldPath}\"");
        return await RunEsptoolAsync(args, progress);
    }

    // Flash the base firmware .bin to the ESP32.
    public static async Task<(bool success, string log)> FlashFirmwareAsync(
        string binPath,
        string? port = null,
        IProgress<string>? progress = null)
    {
        // ESP32-C3 firmware goes to 0x10000 (after bootloader at 0x0 and partition table at 0x8000)
        var args = BuildEsptoolArgs("write_flash", port,
            $"0x0 \"{Path.Combine(Path.GetDirectoryName(binPath)!, "bootloader.bin")}\" " +
            $"0x8000 \"{Path.Combine(Path.GetDirectoryName(binPath)!, "partition-table.bin")}\" " +
            $"0x10000 \"{binPath}\"");
        return await RunEsptoolAsync(args, progress);
    }

    private static string BuildEsptoolArgs(string command, string? port, string extra)
    {
        var portArg = port != null ? $"--port \"{port}\"" : "";
        return $"--chip esp32c3 {portArg} {command} --before default_reset --after hard_reset {extra}";
    }

    private static async Task<(bool success, string log)> RunEsptoolAsync(
        string args,
        IProgress<string>? progress)
    {
        // Try esptool.py first, fall back to esptool (pip install esptool installs both)
        var executable = await FindEsptoolAsync();
        if (executable == null)
            return (false,
                "esptool not found. Please install it with: pip install esptool\n" +
                "Then ensure it is on your PATH.");

        var psi = new ProcessStartInfo
        {
            FileName = executable,
            Arguments = args,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        var log = new System.Text.StringBuilder();
        using var process = new Process { StartInfo = psi };

        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            log.AppendLine(e.Data);
            progress?.Report(e.Data);
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            log.AppendLine(e.Data);
            progress?.Report(e.Data);
        };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();

        return (process.ExitCode == 0, log.ToString());
    }

    private static async Task<string?> FindEsptoolAsync()
    {
        foreach (var candidate in new[] { "esptool.py", "esptool" })
        {
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = candidate,
                    Arguments = "--version",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                using var p = Process.Start(psi);
                if (p != null)
                {
                    await p.WaitForExitAsync();
                    if (p.ExitCode == 0) return candidate;
                }
            }
            catch { }
        }
        return null;
    }
}