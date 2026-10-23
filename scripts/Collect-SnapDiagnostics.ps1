# Read-only diagnostic. Run on the Windows PC where the snap preview lags.
[CmdletBinding()]
param(
    [ValidateRange(5, 60)] [int] $Seconds = 20,
    [string] $OutputPath = (Join-Path $PWD 'snap-diagnostics.json')
)
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Run this script on the affected Windows PC.'
}
if (Test-Path -LiteralPath $OutputPath) {
    throw "Output already exists: $OutputPath. Choose another -OutputPath to preserve it."
}

# No window titles, screenshots, keyboard input, file contents, or browser URLs
# are collected. This only queries visible shell windows and module versions.
if (-not ('SmoothSnap.Diagnostics.NativeWindows' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
namespace SmoothSnap.Diagnostics {
    public class WindowRecord {
        public string Handle;
        public uint ProcessId;
        public string ProcessName;
        public string ClassName;
        public int Left, Top, Right, Bottom;
    }
    public static class NativeWindows {
        private delegate bool EnumProc(IntPtr hwnd, IntPtr data);
        [StructLayout(LayoutKind.Sequential)]
        private struct Rect { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumProc callback, IntPtr data);
        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
        private static readonly HashSet<string> Names = new HashSet<string>(
            new[] { "explorer", "dwm", "ShellExperienceHost", "ShellHost",
                    "StartMenuExperienceHost", "ApplicationFrameHost" },
            StringComparer.OrdinalIgnoreCase);
        public static WindowRecord[] Snapshot() {
            var results = new List<WindowRecord>();
            EnumProc callback = delegate(IntPtr hwnd, IntPtr data) {
                if (!IsWindowVisible(hwnd)) return true;
                uint pid;
                GetWindowThreadProcessId(hwnd, out pid);
                string processName;
                try {
                    using (var process = Process.GetProcessById((int)pid)) {
                        processName = process.ProcessName;
                    }
                } catch (ArgumentException) { return true; }
                  catch (InvalidOperationException) { return true; }
                  catch (System.ComponentModel.Win32Exception) { return true; }
                if (!Names.Contains(processName)) return true;
                var name = new StringBuilder(256);
                Rect rect;
                if (GetClassName(hwnd, name, name.Capacity) == 0 ||
                    !GetWindowRect(hwnd, out rect)) return true;
                results.Add(new WindowRecord {
                    Handle = hwnd.ToInt64().ToString("X"), ProcessId = pid,
                    ProcessName = processName, ClassName = name.ToString(),
                    Left = rect.Left, Top = rect.Top,
                    Right = rect.Right, Bottom = rect.Bottom
                });
                return true;
            };
            if (!EnumWindows(callback, IntPtr.Zero))
                throw new InvalidOperationException("Could not enumerate desktop windows.");
            GC.KeepAlive(callback);
            return results.ToArray();
        }
    }
}
'@
}

$version = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$files = @('explorer.exe', 'System32\twinui.pcshell.dll',
           'System32\Windows.UI.Composition.dll', 'System32\udwm.dll')
$versions = foreach ($relativePath in $files) {
    $path = Join-Path $env:SystemRoot $relativePath
    if (Test-Path -LiteralPath $path) {
        $info = (Get-Item -LiteralPath $path).VersionInfo
        [ordered]@{ File = $relativePath; Version = $info.FileVersion }
    }
}
$frames = [System.Collections.Generic.List[object]]::new()
Write-Host "For the next $Seconds seconds, drag a window across the snap targets."
Write-Host 'The script only observes shell window classes and rectangles. It changes no settings.'
$clock = [System.Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $frames.Add([ordered]@{
        ElapsedMs = $clock.ElapsedMilliseconds
        Windows = @([SmoothSnap.Diagnostics.NativeWindows]::Snapshot())
    })
    Start-Sleep -Milliseconds 100
}
$clock.Stop()
$report = [ordered]@{
    SchemaVersion = 1
    Edition = $version.EditionID
    DisplayVersion = $version.DisplayVersion
    Build = ('{0}.{1}' -f $version.CurrentBuildNumber, $version.UBR)
    Is64BitOS = [Environment]::Is64BitOperatingSystem
    Is64BitPowerShell = [Environment]::Is64BitProcess
    SystemFiles = @($versions)
    Sampling = 'Approximately 10 Hz; coordinates may be DPI virtualized. Not a frame-rate measurement.'
    Frames = $frames.ToArray()
}
# CreateNew also protects a file created by someone else during capture.
$json = $report | ConvertTo-Json -Depth 8
$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream.Write($bytes, 0, $bytes.Length)
} finally { $stream.Dispose() }
Write-Host "Saved $OutputPath. Share this JSON alongside your mod debug log."
