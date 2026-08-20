using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;

namespace StarImageViewer;

internal static class ThumbnailCache
{
    private const long MaximumCacheBytes = 500L * 1024 * 1024;
    private const string CacheVersion = "v2";
    private static readonly string CacheDirectory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AstraView", "ThumbnailCache");
    private static int cleanupStarted;
    private static readonly SemaphoreSlim WriterGate = new(1, 1);

    public static BitmapSource? TryGet(string sourcePath, int requestedSize)
    {
        StartCleanupOnce();
        var cachePath = GetCachePath(sourcePath, requestedSize);
        if (!File.Exists(cachePath)) return null;
        try
        {
            using var stream = new FileStream(cachePath, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete, 32768, FileOptions.SequentialScan);
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.StreamSource = stream;
            bitmap.EndInit();
            bitmap.Freeze();
            return bitmap;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (NotSupportedException) { return null; }
    }

    public static void Store(string sourcePath, int requestedSize, BitmapSource bitmap)
    {
        try
        {
            Directory.CreateDirectory(CacheDirectory);
            var destination = GetCachePath(sourcePath, requestedSize);
            if (File.Exists(destination)) return;
            var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                           FileShare.None, 32768, FileOptions.SequentialScan))
                    encoder.Save(stream);
                if (!File.Exists(destination)) File.Move(temporary, destination);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    public static async Task StoreInBackgroundAsync(string sourcePath, int requestedSize, BitmapSource bitmap)
    {
        await WriterGate.WaitAsync();
        try { await Task.Run(() => Store(sourcePath, requestedSize, bitmap)); }
        finally { WriterGate.Release(); }
    }

    private static string GetCachePath(string sourcePath, int requestedSize)
    {
        var info = new FileInfo(sourcePath);
        var identity = string.Join("|", CacheVersion, Path.GetFullPath(sourcePath).ToUpperInvariant(),
            info.Length, info.LastWriteTimeUtc.Ticks, requestedSize);
        using var sha = SHA256.Create();
        var hash = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(identity))).Replace("-", string.Empty);
        return Path.Combine(CacheDirectory, hash + ".png");
    }

    private static void StartCleanupOnce()
    {
        if (Interlocked.Exchange(ref cleanupStarted, 1) != 0) return;
        _ = Task.Run(Cleanup);
    }

    private static void Cleanup()
    {
        try
        {
            if (!Directory.Exists(CacheDirectory)) return;
            var files = new DirectoryInfo(CacheDirectory).EnumerateFiles("*.png")
                .OrderByDescending(file => file.LastWriteTimeUtc).ToList();
            long retained = 0;
            foreach (var file in files)
            {
                retained += file.Length;
                if (retained > MaximumCacheBytes) file.Delete();
            }
            foreach (var temporary in new DirectoryInfo(CacheDirectory).EnumerateFiles("*.tmp"))
                if (temporary.LastWriteTimeUtc < DateTime.UtcNow.AddDays(-1)) temporary.Delete();
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
