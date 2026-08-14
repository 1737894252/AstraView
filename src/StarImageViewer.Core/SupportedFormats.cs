using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace StarImageViewer.Core;

public static class SupportedFormats
{
    public static readonly string[] Extensions =
    {
        ".3fr", ".arw", ".avif", ".bmp", ".cr2", ".cr3", ".crw", ".dcr", ".dds",
        ".dng", ".emf", ".erf", ".exr", ".gif", ".heic", ".heif", ".ico", ".jfif",
        ".jpe", ".jpeg", ".jpg", ".jxl", ".kdc", ".miff", ".mos", ".mrw", ".nef",
        ".nrw", ".orf", ".pbm", ".pcx", ".pef", ".pgm", ".png", ".pnm", ".ppm",
        ".psb", ".psd", ".raf", ".raw", ".rw2", ".rwl", ".sgi", ".sr2", ".srf",
        ".svg", ".svgz", ".tga", ".tif", ".tiff", ".webp", ".wmf", ".x3f", ".xbm", ".xpm",
        ".pdf"
    };

    private static readonly HashSet<string> ExtensionSet =
        new(Extensions, StringComparer.OrdinalIgnoreCase);

    public static bool IsSupported(string path) => ExtensionSet.Contains(Path.GetExtension(path));

    public static string FileDialogFilter =>
        $"所有支持的图片|{string.Join(";", Extensions.Select(x => "*" + x))}|所有文件|*.*";
}
