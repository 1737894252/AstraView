using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace StarImageViewer.ThumbnailProvider;

internal static class NativeBitmap
{
    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfoHeader
    {
        public uint Size;
        public int Width;
        public int Height;
        public ushort Planes;
        public ushort BitCount;
        public uint Compression;
        public uint SizeImage;
        public int XPelsPerMeter;
        public int YPelsPerMeter;
        public uint ColorsUsed;
        public uint ColorsImportant;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfo
    {
        public BitmapInfoHeader Header;
        public uint Colors;
    }

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateDIBSection(IntPtr hdc, ref BitmapInfo info, uint usage,
        out IntPtr bits, IntPtr section, uint offset);

    public static IntPtr FromBgra(byte[] pixels, uint width, uint height)
    {
        var expected = checked((int)(width * height * 4));
        if (pixels.Length != expected) throw new ArgumentException("Unexpected BGRA pixel buffer size.");
        var info = new BitmapInfo
        {
            Header = new BitmapInfoHeader
            {
                Size = (uint)Marshal.SizeOf(typeof(BitmapInfoHeader)),
                Width = checked((int)width),
                Height = -checked((int)height),
                Planes = 1,
                BitCount = 32,
                Compression = 0,
                SizeImage = (uint)expected
            }
        };
        var bitmap = CreateDIBSection(IntPtr.Zero, ref info, 0, out var bits, IntPtr.Zero, 0);
        if (bitmap == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        Marshal.Copy(pixels, 0, bits, pixels.Length);
        return bitmap;
    }
}
