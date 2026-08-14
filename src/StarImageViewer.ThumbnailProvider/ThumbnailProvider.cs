using Microsoft.Win32;
using StarImageViewer.Core;
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace StarImageViewer.ThumbnailProvider;

[ComVisible(true)]
[Guid(ClassId)]
[ClassInterface(ClassInterfaceType.None)]
public sealed class ThumbnailProvider : IInitializeWithStream, IThumbnailProvider
{
    public const string ClassId = "5E2D8E48-6F15-4C3D-AED8-BDA6544D2253";
    private const string ThumbnailHandlerId = "{e357fccd-a995-4576-b01f-234630154e96}";
    private MemoryStream? input;

    public void Initialize(IStream stream, uint grfMode)
    {
        if (input != null) Marshal.ThrowExceptionForHR(unchecked((int)0x800704DF));
        input = CopyStream(stream);
    }

    public void GetThumbnail(uint cx, out IntPtr hBitmap, out WtsAlphaType bitmapType)
    {
        hBitmap = IntPtr.Zero;
        bitmapType = WtsAlphaType.Unknown;
        var source = input;
        if (source == null) Marshal.ThrowExceptionForHR(unchecked((int)0x8000FFFF));
        try
        {
            var size = ImageDecoder.GetThumbnailSize(source!, Math.Min(cx, 2048));
            var pixels = ImageDecoder.DecodeThumbnail(source!, Math.Min(cx, 2048));
            hBitmap = NativeBitmap.FromBgra(pixels, size.Width, size.Height);
            bitmapType = WtsAlphaType.Argb;
        }
        catch (Exception ex)
        {
            throw new COMException("Image thumbnail decoding failed: " + ex.Message, unchecked((int)0x80004005));
        }
    }

    private static MemoryStream CopyStream(IStream source)
    {
        var result = new MemoryStream();
        var buffer = new byte[64 * 1024];
        var readPtr = Marshal.AllocCoTaskMem(sizeof(int));
        try
        {
            while (true)
            {
                source.Read(buffer, buffer.Length, readPtr);
                var read = Marshal.ReadInt32(readPtr);
                if (read <= 0) break;
                result.Write(buffer, 0, read);
            }
            result.Position = 0;
            return result;
        }
        finally { Marshal.FreeCoTaskMem(readPtr); }
    }

    [ComRegisterFunction]
    public static void Register(Type type)
    {
        using (var approved = Registry.LocalMachine.CreateSubKey(
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"))
        {
            approved?.SetValue("{" + ClassId + "}", "StarImageViewer Thumbnail Provider");
        }
        foreach (var extension in SupportedFormats.Extensions)
        {
            using var key = Registry.LocalMachine.CreateSubKey(
                $@"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\KindMap");
            key?.SetValue(extension, "picture");
            using var handler = Registry.LocalMachine.CreateSubKey(
                $@"SOFTWARE\Classes\SystemFileAssociations\{extension}\shellex\{ThumbnailHandlerId}");
            handler?.SetValue(null, "{" + ClassId + "}");
        }
    }

    [ComUnregisterFunction]
    public static void Unregister(Type type)
    {
        using (var approved = Registry.LocalMachine.OpenSubKey(
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved", true))
        {
            approved?.DeleteValue("{" + ClassId + "}", false);
        }
        foreach (var extension in SupportedFormats.Extensions)
        {
            Registry.LocalMachine.DeleteSubKey(
                $@"SOFTWARE\Classes\SystemFileAssociations\{extension}\shellex\{ThumbnailHandlerId}", false);
        }
    }
}
