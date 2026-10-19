using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace BakeCacheMaintenance;

internal static class WindowsFiles
{
    internal readonly record struct Identity(uint Volume, ulong Index, uint Links);

    [StructLayout(LayoutKind.Sequential)]
    private struct Information
    {
        public uint Attributes;
        public FILETIME Creation, Access, Write;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Information information);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(string path, string existing, IntPtr security);

    internal static Identity Identify(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        if (!GetFileInformationByHandle(stream.SafeFileHandle, out var value))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return new(value.Volume, ((ulong)value.IndexHigh << 32) | value.IndexLow, value.Links);
    }

    internal static void Link(string path, string existing)
    {
        if (!CreateHardLinkW(path, existing, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
