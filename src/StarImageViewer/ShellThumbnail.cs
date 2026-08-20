using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace StarImageViewer;

internal static class ShellThumbnail
{
    private static readonly Guid ShellItemImageFactoryId =
        new("bcc18b79-ba16-442f-80c4-8a59c30c463b");

    public static BitmapSource? TryGetFromMemoryCache(string path, int requestedSize)
    {
        IShellItemImageFactory? factory = null;
        IntPtr bitmapHandle = IntPtr.Zero;
        try
        {
            var interfaceId = ShellItemImageFactoryId;
            var result = SHCreateItemFromParsingName(path, IntPtr.Zero, ref interfaceId, out factory);
            if (result < 0 || factory == null) return null;
            var size = new NativeSize(requestedSize, requestedSize);
            result = factory.GetImage(size,
                ShellImageFlags.ThumbnailOnly | ShellImageFlags.InCacheOnly | ShellImageFlags.BiggerSizeOk,
                out bitmapHandle);
            if (result < 0 || bitmapHandle == IntPtr.Zero) return null;
            var source = Imaging.CreateBitmapSourceFromHBitmap(bitmapHandle, IntPtr.Zero,
                Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
            source.Freeze();
            return source;
        }
        catch (COMException) { return null; }
        finally
        {
            if (bitmapHandle != IntPtr.Zero) DeleteObject(bitmapHandle);
            if (factory != null && Marshal.IsComObject(factory)) Marshal.FinalReleaseComObject(factory);
        }
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHCreateItemFromParsingName(
        string path, IntPtr bindingContext, ref Guid interfaceId,
        [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory factory);

    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeleteObject(IntPtr value);

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b")]
    private interface IShellItemImageFactory
    {
        [PreserveSig]
        int GetImage(NativeSize size, ShellImageFlags flags, out IntPtr bitmapHandle);
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct NativeSize
    {
        public NativeSize(int width, int height) { Width = width; Height = height; }
        public readonly int Width;
        public readonly int Height;
    }

    [Flags]
    private enum ShellImageFlags
    {
        BiggerSizeOk = 0x1,
        ThumbnailOnly = 0x8,
        InCacheOnly = 0x10
    }
}
