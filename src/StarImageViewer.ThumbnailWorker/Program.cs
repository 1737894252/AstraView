using StarImageViewer.Core;
using System;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AstraView.ThumbnailWorker;

internal static class Program
{
    private const string PipeName = "AstraView.ThumbnailWorker.v1";
    private const int MaxThumbnailBytes = 2048 * 2048 * 4;
    private static long lastRequestTicks = DateTime.UtcNow.Ticks;

    private static async Task<int> Main()
    {
        using var singleInstance = new Mutex(true, @"Local\AstraView.ThumbnailWorker.v1", out var ownsMutex);
        if (!ownsMutex) return 0;

        using var stopping = new CancellationTokenSource();
        var servers = Enumerable.Range(0, Math.Clamp(Environment.ProcessorCount / 2, 2, 4))
            .Select(_ => ServeAsync(stopping.Token)).ToArray();

        while (!stopping.IsCancellationRequested)
        {
            await Task.Delay(TimeSpan.FromSeconds(15));
            if (new TimeSpan(DateTime.UtcNow.Ticks - Interlocked.Read(ref lastRequestTicks)) > TimeSpan.FromMinutes(2))
                stopping.Cancel();
        }

        try { await Task.WhenAll(servers); } catch (OperationCanceledException) { }
        return 0;
    }

    private static async Task ServeAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 4,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
            await pipe.WaitForConnectionAsync(cancellationToken);
            Interlocked.Exchange(ref lastRequestTicks, DateTime.UtcNow.Ticks);
            await ProcessRequestAsync(pipe, cancellationToken);
        }
    }

    private static async Task ProcessRequestAsync(Stream stream, CancellationToken cancellationToken)
    {
        using var reader = new BinaryReader(stream, System.Text.Encoding.Unicode, leaveOpen: true);
        using var writer = new BinaryWriter(stream, System.Text.Encoding.Unicode, leaveOpen: true);
        try
        {
            var requestedSize = reader.ReadUInt32();
            var pathLength = reader.ReadInt32();
            if (requestedSize is 0 or > 2048 || pathLength is <= 0 or > 32767)
                throw new InvalidDataException("Invalid thumbnail request.");

            var pathBytes = reader.ReadBytes(checked(pathLength * 2));
            if (pathBytes.Length != pathLength * 2) throw new EndOfStreamException();
            var path = System.Text.Encoding.Unicode.GetString(pathBytes);
            var result = IsWindowsCodecFormat(path)
                ? DecodeWithWindowsImaging(path, checked((int)requestedSize))
                : ImageDecoder.DecodeForDisplayWithSize(path, requestedSize);
            if (result.Pixels.Length > MaxThumbnailBytes) throw new InvalidDataException("Thumbnail is too large.");

            writer.Write(0);
            writer.Write(result.Width);
            writer.Write(result.Height);
            writer.Write(result.Pixels.Length);
            writer.Write(result.Pixels);
        }
        catch
        {
            writer.Write(unchecked((int)0x80004005));
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(0);
        }
        await stream.FlushAsync(cancellationToken);
    }

    private static bool IsWindowsCodecFormat(string path) => Path.GetExtension(path).ToLowerInvariant() is
        ".png" or ".jpg" or ".jpeg" or ".jpe" or ".jfif" or ".bmp" or ".gif" or ".ico" or ".tif" or ".tiff";

    private static ImageDecoder.ThumbnailData DecodeWithWindowsImaging(string path, int requestedSize)
    {
        int originalWidth;
        int originalHeight;
        using (var header = new FileStream(path, FileMode.Open, FileAccess.Read,
                   FileShare.ReadWrite | FileShare.Delete))
        {
            var decoder = BitmapDecoder.Create(header, BitmapCreateOptions.DelayCreation, BitmapCacheOption.None);
            originalWidth = decoder.Frames[0].PixelWidth;
            originalHeight = decoder.Frames[0].PixelHeight;
        }

        using var input = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete, 65536, FileOptions.SequentialScan);
        var bitmap = new BitmapImage();
        bitmap.BeginInit();
        bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.StreamSource = input;
        if (originalWidth >= originalHeight) bitmap.DecodePixelWidth = requestedSize;
        else bitmap.DecodePixelHeight = requestedSize;
        bitmap.EndInit();
        bitmap.Freeze();

        BitmapSource bgra = bitmap.Format == PixelFormats.Bgra32
            ? bitmap
            : new FormatConvertedBitmap(bitmap, PixelFormats.Bgra32, null, 0);
        var stride = checked(bgra.PixelWidth * 4);
        var pixels = new byte[checked(stride * bgra.PixelHeight)];
        bgra.CopyPixels(pixels, stride, 0);
        return new ImageDecoder.ThumbnailData(pixels, checked((uint)bgra.PixelWidth),
            checked((uint)bgra.PixelHeight), checked((uint)originalWidth), checked((uint)originalHeight));
    }
}
