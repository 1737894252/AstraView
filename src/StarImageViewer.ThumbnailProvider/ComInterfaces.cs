using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace StarImageViewer.ThumbnailProvider;

[ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f")]
public interface IInitializeWithStream
{
    void Initialize([MarshalAs(UnmanagedType.Interface)] IStream stream, uint grfMode);
}

[ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("e357fccd-a995-4576-b01f-234630154e96")]
public interface IThumbnailProvider
{
    void GetThumbnail(uint cx, out IntPtr hBitmap, out WtsAlphaType bitmapType);
}

public enum WtsAlphaType
{
    Unknown = 0,
    Rgb = 1,
    Argb = 2
}
