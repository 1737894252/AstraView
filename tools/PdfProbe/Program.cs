using StarImageViewer.Core;
using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Diagnostics;

internal static class Program
{
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    private static extern void SHCreateStreamOnFileEx(string fileName, uint mode,
        uint attributes, bool create, IStream? template, out IStream stream);

    [DllImport("gdi32.dll")]
    private static extern bool DeleteObject(IntPtr value);

    private static void Main(string[] args)
    {
        var bytes = ImageDecoder.DecodeForDisplay(args[0], 2048);
        File.WriteAllBytes(args[1], bytes);
        using (var stream = new MemoryStream(bytes))
        using (var image = Image.FromStream(stream))
            Console.WriteLine($"{image.Width}x{image.Height} {bytes.Length} bytes");

        using (var pdf = File.OpenRead(args[0]))
        {
            var size = ImageDecoder.GetThumbnailSize(pdf, 256);
            var pixels = ImageDecoder.DecodeThumbnail(pdf, 256);
            Console.WriteLine($"thumbnail {size.Width}x{size.Height} {pixels.Length} BGRA bytes");
        }

        SHCreateStreamOnFileEx(Path.GetFullPath(args[0]), 0, 0, false, null, out var shellStream);
        var provider = new StarImageViewer.ThumbnailProvider.ThumbnailProvider();
        provider.Initialize(shellStream, 0);
        var stopwatch = Stopwatch.StartNew();
        provider.GetThumbnail(256, out var bitmap, out var alphaType);
        stopwatch.Stop();
        try { Console.WriteLine($"shell stream thumbnail OK in {stopwatch.ElapsedMilliseconds} ms ({alphaType})"); }
        finally { if (bitmap != IntPtr.Zero) DeleteObject(bitmap); }
    }
}
