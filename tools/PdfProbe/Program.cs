using StarImageViewer.Core;
using System;
using System.Drawing;
using System.IO;

internal static class Program
{
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
    }
}
