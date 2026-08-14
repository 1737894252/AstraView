using ImageMagick;
using PDFtoImage;
using System;
using System.IO;

namespace StarImageViewer.Core;

public static class ImageDecoder
{
    public sealed class ThumbnailData
    {
        public ThumbnailData(byte[] pixels, uint width, uint height)
        {
            Pixels = pixels;
            Width = width;
            Height = height;
        }

        public byte[] Pixels { get; }
        public uint Width { get; }
        public uint Height { get; }
    }

    public static byte[] DecodeForDisplay(string path, int maxDimension = 8192)
    {
        if (Path.GetExtension(path).Equals(".pdf", StringComparison.OrdinalIgnoreCase))
        {
            using var pdf = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            return RenderPdfFirstPage(pdf, (uint)Math.Min(maxDimension, 4096));
        }
        using var image = ReadFirstUsefulFrame(path);
        Prepare(image, checked((uint)maxDimension), checked((uint)maxDimension));
        image.Format = MagickFormat.Png32;
        return image.ToByteArray();
    }

    public static byte[] DecodeThumbnail(Stream input, uint requestedSize)
    {
        return DecodeThumbnailWithSize(input, requestedSize).Pixels;
    }

    public static (uint Width, uint Height) GetThumbnailSize(Stream input, uint requestedSize)
    {
        var thumbnail = DecodeThumbnailWithSize(input, requestedSize);
        return (thumbnail.Width, thumbnail.Height);
    }

    public static ThumbnailData DecodeThumbnailWithSize(Stream input, uint requestedSize)
    {
        if (requestedSize == 0) throw new ArgumentOutOfRangeException(nameof(requestedSize));
        using var image = ReadFirstUsefulFrame(input, requestedSize);
        Prepare(image, requestedSize, requestedSize);
        var width = image.Width;
        var height = image.Height;
        image.Format = MagickFormat.Bgra;
        return new ThumbnailData(image.ToByteArray(), width, height);
    }

    private static MagickImage ReadFirstUsefulFrame(string path)
    {
        var settings = new MagickReadSettings { FrameIndex = 0, FrameCount = 1 };
        return new MagickImage(path, settings);
    }

    private static MagickImage ReadFirstUsefulFrame(Stream input, uint pdfRenderWidth)
    {
        if (input.CanSeek) input.Position = 0;
        if (IsPdf(input))
        {
            var rendered = RenderPdfFirstPage(input, pdfRenderWidth);
            return new MagickImage(rendered);
        }
        var settings = new MagickReadSettings { FrameIndex = 0, FrameCount = 1 };
        return new MagickImage(input, settings);
    }

    private static bool IsPdf(Stream input)
    {
        if (!input.CanSeek) return false;
        var originalPosition = input.Position;
        try
        {
            input.Position = 0;
            var signature = new byte[5];
            return input.Read(signature, 0, signature.Length) == signature.Length &&
                   signature[0] == '%' && signature[1] == 'P' && signature[2] == 'D' &&
                   signature[3] == 'F' && signature[4] == '-';
        }
        finally { input.Position = originalPosition; }
    }

    private static byte[] RenderPdfFirstPage(Stream pdf, uint targetWidth)
    {
        if (pdf.CanSeek) pdf.Position = 0;
        using var output = new MemoryStream();
        var options = new RenderOptions
        {
            Width = checked((int)Math.Max(1, targetWidth)),
            Height = null,
            WithAspectRatio = true,
            WithAnnotations = true,
            AntiAliasing = PdfAntiAliasing.All
        };
        Conversion.SavePng(output, pdf, 0, true, null, options);
        return output.ToArray();
    }

    private static void Prepare(MagickImage image, uint maxWidth, uint maxHeight)
    {
        image.AutoOrient();
        image.ColorSpace = ColorSpace.sRGB;
        image.Strip();
        if (image.Width > maxWidth || image.Height > maxHeight)
        {
            image.FilterType = FilterType.Lanczos;
            image.Resize(new MagickGeometry(maxWidth, maxHeight) { IgnoreAspectRatio = false });
        }
    }
}
