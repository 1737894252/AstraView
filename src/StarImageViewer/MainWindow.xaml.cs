using Microsoft.Win32;
using StarImageViewer.Core;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace StarImageViewer;

public partial class MainWindow : Window
{
    private const int FolderThumbnailDecodeSize = 96;
    private static readonly int StandardThumbnailConcurrency =
        Math.Max(2, Math.Min(6, Environment.ProcessorCount - 1));
    private static readonly SemaphoreSlim StandardThumbnailGate =
        new(StandardThumbnailConcurrency, StandardThumbnailConcurrency);
    private static readonly SemaphoreSlim HeavyThumbnailGate = new(1, 1);
    private sealed class DecodedBitmap
    {
        public required BitmapSource Bitmap { get; init; }
        public int OriginalWidth { get; init; }
        public int OriginalHeight { get; init; }
    }
    private string? currentPath;
    private List<string> siblings = new();
    private double zoom = 1;
    private Point dragStart;
    private Point panStart;
    private bool dragging;
    private CancellationTokenSource? thumbnailCancellation;
    private CancellationTokenSource? folderRefreshCancellation;
    private CancellationTokenSource? imageLoadCancellation;
    private int originalPixelWidth;
    private int originalPixelHeight;
    private FileSystemWatcher? folderWatcher;
    private string? watchedFolder;
    private string? navigationRootFolder;
    private readonly List<string> recentFolders = new();
    private static readonly string RecentFoldersPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AstraView", "recent-folders.txt");

    public MainWindow()
    {
        InitializeComponent();
        LoadRecentFolders();
        SizeChanged += (_, __) => UpdateTitleLayout();
        SourceInitialized += (_, __) =>
        {
            if (PresentationSource.FromVisual(this) is HwndSource source)
                source.AddHook(WindowProc);
        };
        Viewport.SizeChanged += (_, __) => CenterPicture();
        Loaded += async (_, __) =>
        {
            UpdateTitleLayout();
            var args = Environment.GetCommandLineArgs();
            if (args.Length > 1 && File.Exists(args[1])) await LoadImageAsync(args[1]);
            else if (args.Length > 1 && Directory.Exists(args[1])) await LoadDirectoryAsync(args[1]);
        };
        Closed += (_, __) =>
        {
            thumbnailCancellation?.Cancel();
            folderRefreshCancellation?.Cancel();
            imageLoadCancellation?.Cancel();
            folderWatcher?.Dispose();
        };
    }

    private void UpdateTitleLayout()
    {
        if (TitleBarGrid.ActualWidth <= 0) return;
        const double safetyMargin = 14;
        var centre = TitleBarGrid.ActualWidth / 2;
        var safeLeft = LeftCommands.ActualWidth + safetyMargin;
        var safeRight = TitleBarGrid.ActualWidth - WindowCommands.ActualWidth - safetyMargin;
        var halfAvailable = Math.Min(centre - safeLeft, safeRight - centre);
        TitleText.MaxWidth = Math.Max(0, halfAvailable * 2);
        TitleText.Visibility = halfAvailable >= 24 ? Visibility.Visible : Visibility.Collapsed;
    }

    private async Task LoadImageAsync(string path)
    {
        imageLoadCancellation?.Cancel();
        var cancellation = new CancellationTokenSource();
        imageLoadCancellation = cancellation;
        var token = cancellation.Token;
        try
        {
            StatusText.Text = "正在加载…";
            // The viewer displays the original decoded pixel dimensions in a single pass. This avoids
            // replacing a low-resolution preview with a second bitmap after the image is already visible.
            var decoded = await Task.Run(() => DecodeBitmap(path, int.MaxValue), token);
            token.ThrowIfCancellationRequested();
            var bitmap = decoded.Bitmap;
            Picture.Source = bitmap;
            // Give the Image its full pixel-sized layout box. If it is stretched to the viewport,
            // WPF clips oversized sources before the render transform and "fit" shows only the centre.
            Picture.Width = bitmap.PixelWidth;
            Picture.Height = bitmap.PixelHeight;
            CenterPicture();
            RotateTransform.Angle = 0;
            PanTransform.X = PanTransform.Y = 0;
            originalPixelWidth = decoded.OriginalWidth;
            originalPixelHeight = decoded.OriginalHeight;
            currentPath = Path.GetFullPath(path);
            TitleText.Text = Path.GetFileName(path);
            Title = $"{Path.GetFileName(path)} — AstraView";
            EmptyHint.Visibility = Visibility.Collapsed;
            RefreshSiblings();
            StatusText.Text = BuildImageStatus(path);
            FitToWindow();
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            StatusText.Text = "无法解码";
            MessageBox.Show($"无法打开图片：\n{path}\n\n{ex.Message}", "AstraView", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private string BuildImageStatus(string path)
    {
        var status = $"{originalPixelWidth} × {originalPixelHeight}   {FormatBytes(new FileInfo(path).Length)}";
        var position = siblings.FindIndex(x => string.Equals(x, path, StringComparison.OrdinalIgnoreCase));
        if (position >= 0) status += $"   {position + 1} / {siblings.Count}";
        return status;
    }

    private static DecodedBitmap DecodeBitmap(string path, int maxDimension)
    {
        // WIC handles these formats natively and avoids Magick decode -> PNG encode -> WPF decode.
        var extension = Path.GetExtension(path);
        if (extension.Equals(".pdf", StringComparison.OrdinalIgnoreCase)) maxDimension = 4096;
        if (extension.Equals(".png", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpeg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpe", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jfif", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".bmp", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".gif", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".ico", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".tif", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".tiff", StringComparison.OrdinalIgnoreCase))
            return DecodeWithWindowsImaging(path, maxDimension);

        var decoded = ImageDecoder.DecodeForDisplayWithSize(path, checked((uint)maxDimension));
        var converted = BitmapSource.Create(checked((int)decoded.Width), checked((int)decoded.Height),
            96, 96, PixelFormats.Bgra32, null, decoded.Pixels, checked((int)decoded.Width * 4));
        converted.Freeze();
        return new DecodedBitmap
        {
            Bitmap = converted,
            OriginalWidth = checked((int)decoded.OriginalWidth),
            OriginalHeight = checked((int)decoded.OriginalHeight)
        };
    }

    private static DecodedBitmap DecodeWithWindowsImaging(string path, int maxDimension)
    {
        int pixelWidth;
        int pixelHeight;
        using (var headerStream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        {
            var decoder = BitmapDecoder.Create(headerStream, BitmapCreateOptions.DelayCreation,
                                               BitmapCacheOption.None);
            pixelWidth = decoder.Frames[0].PixelWidth;
            pixelHeight = decoder.Frames[0].PixelHeight;
        }

        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var bitmap = new BitmapImage();
        bitmap.BeginInit();
        bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.CreateOptions = BitmapCreateOptions.PreservePixelFormat;
        bitmap.StreamSource = stream;
        if (pixelWidth > maxDimension || pixelHeight > maxDimension)
        {
            if (pixelWidth >= pixelHeight) bitmap.DecodePixelWidth = maxDimension;
            else bitmap.DecodePixelHeight = maxDimension;
        }
        bitmap.EndInit();
        bitmap.Freeze();
        return new DecodedBitmap { Bitmap = bitmap, OriginalWidth = pixelWidth, OriginalHeight = pixelHeight };
    }

    private void RefreshSiblings()
    {
        if (currentPath == null) return;
        if (watchedFolder != null && IsPathInsideFolder(currentPath, watchedFolder)) return;
        var folder = Path.GetDirectoryName(currentPath)!;
        try
        {
            siblings = Directory.EnumerateFiles(folder).Where(SupportedFormats.IsSupported)
                .OrderBy(x => x, StringComparer.CurrentCultureIgnoreCase).ToList();
        }
        catch { siblings = new List<string> { currentPath }; }
    }

    private async Task NavigateAsync(int delta)
    {
        if (currentPath == null || siblings.Count == 0) return;
        var index = siblings.FindIndex(x => string.Equals(x, currentPath, StringComparison.OrdinalIgnoreCase));
        if (index < 0) return;
        await LoadImageAsync(siblings[(index + delta + siblings.Count) % siblings.Count]);
    }

    private async Task LoadDirectoryAsync(string folder, bool setAsNavigationRoot = true)
    {
        List<string> images;
        List<string> subfolders;
        try
        {
            folder = Path.GetFullPath(folder);
            var snapshot = await Task.Run(() => new
            {
                Images = Directory.EnumerateFiles(folder).Where(SupportedFormats.IsSupported)
                    .OrderBy(x => x, StringComparer.CurrentCultureIgnoreCase).ToList(),
                Subfolders = EnumerateBrowsableDirectories(folder)
            });
            images = snapshot.Images;
            subfolders = snapshot.Subfolders;
        }
        catch (Exception ex)
        {
            MessageBox.Show($"无法读取文件夹：\n{folder}\n\n{ex.Message}", "AstraView", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        if (images.Count == 0 && subfolders.Count == 0)
        {
            MessageBox.Show("这个文件夹中没有图片或子文件夹。", "AstraView", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        if (setAsNavigationRoot)
        {
            navigationRootFolder = folder;
            RememberFolder(folder);
        }
        siblings = images;
        SetupFolderWatcher(folder);
        thumbnailCancellation?.Cancel();
        thumbnailCancellation = new CancellationTokenSource();
        FolderNameText.Text = new DirectoryInfo(folder).Name;
        FolderPanel.Visibility = Visibility.Visible;
        FilmstripRow.Height = new GridLength(128);
        ThumbnailPanel.Children.Clear();
        PopulateDirectoryEntries(folder, subfolders);
        // Start the filmstrip before decoding the first full-size image. A highly compressed PNG can
        // be only a few megabytes on disk yet expand to hundreds of megabytes, and must not block the
        // user's first visual feedback from the folder.
        _ = PopulateThumbnailsAsync(images, thumbnailCancellation.Token);
        if (images.Count > 0) await LoadImageAsync(images[0]);
        else ClearPictureForFolder();
    }

    private void SetupFolderWatcher(string folder)
    {
        if (string.Equals(watchedFolder, folder, StringComparison.OrdinalIgnoreCase)) return;
        folderWatcher?.Dispose();
        watchedFolder = folder;
        folderWatcher = new FileSystemWatcher(folder)
        {
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite | NotifyFilters.Size,
            IncludeSubdirectories = false,
            EnableRaisingEvents = true
        };
        folderWatcher.Created += (_, __) => ScheduleFolderRefresh();
        folderWatcher.Deleted += (_, __) => ScheduleFolderRefresh();
        folderWatcher.Changed += (_, __) => ScheduleFolderRefresh();
        folderWatcher.Renamed += (_, __) => ScheduleFolderRefresh();
    }

    private void ScheduleFolderRefresh()
    {
        var folder = watchedFolder;
        if (folder == null) return;
        folderRefreshCancellation?.Cancel();
        var cancellation = new CancellationTokenSource();
        folderRefreshCancellation = cancellation;
        _ = RefreshFolderAfterDelayAsync(folder, cancellation.Token);
    }

    private async Task RefreshFolderAfterDelayAsync(string folder, CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(650, cancellationToken);
            var images = await Task.Run(() => Directory.EnumerateFiles(folder)
                .Where(SupportedFormats.IsSupported)
                .OrderBy(x => x, StringComparer.CurrentCultureIgnoreCase).ToList(), cancellationToken);
            var subfolders = await Task.Run(() => EnumerateBrowsableDirectories(folder), cancellationToken);
            if (cancellationToken.IsCancellationRequested ||
                !string.Equals(folder, watchedFolder, StringComparison.OrdinalIgnoreCase)) return;
            await Dispatcher.InvokeAsync(() => ApplyFolderSnapshot(folder, images, subfolders));
        }
        catch (OperationCanceledException) { }
        catch (IOException) { ScheduleFolderRefresh(); }
        catch (UnauthorizedAccessException) { }
    }

    private void ApplyFolderSnapshot(string folder, List<string> images, List<string> subfolders)
    {
        siblings = images;
        thumbnailCancellation?.Cancel();
        thumbnailCancellation = new CancellationTokenSource();
        ThumbnailPanel.Children.Clear();
        PopulateDirectoryEntries(folder, subfolders);
        _ = PopulateThumbnailsAsync(images, thumbnailCancellation.Token);

        var position = currentPath == null ? -1 : images.FindIndex(x =>
            string.Equals(x, currentPath, StringComparison.OrdinalIgnoreCase));
        if (position < 0)
        {
            if (images.Count > 0) _ = LoadImageAsync(images[0]);
            else ClearPictureForFolder();
            return;
        }

        if (Picture.Source is BitmapSource && currentPath != null && File.Exists(currentPath))
            StatusText.Text = BuildImageStatus(currentPath);
    }

    private static List<string> EnumerateBrowsableDirectories(string folder)
    {
        try
        {
            return Directory.EnumerateDirectories(folder)
                .Where(path => (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0)
                .OrderBy(path => path, StringComparer.CurrentCultureIgnoreCase).ToList();
        }
        catch (UnauthorizedAccessException) { return new List<string>(); }
        catch (IOException) { return new List<string>(); }
    }

    private void PopulateDirectoryEntries(string folder, IEnumerable<string> subfolders)
    {
        if (navigationRootFolder != null && !string.Equals(folder, navigationRootFolder, StringComparison.OrdinalIgnoreCase))
            AddDirectoryButton(Path.GetDirectoryName(folder)!, "返回上一级", true);
        foreach (var subfolder in subfolders)
            AddDirectoryButton(subfolder, new DirectoryInfo(subfolder).Name, false);
    }

    private void AddDirectoryButton(string folder, string caption, bool isParent)
    {
        var content = new StackPanel { Width = 82 };
        var folderIcon = new Grid { Width = 78, Height = 64 };
        folderIcon.Children.Add(new Border
        {
            Width = 76, Height = 60, CornerRadius = new CornerRadius(7),
            Background = new SolidColorBrush(Color.FromRgb(31, 36, 44)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(49, 56, 68)),
            BorderThickness = new Thickness(1)
        });
        folderIcon.Children.Add(new System.Windows.Shapes.Path
        {
            Data = Geometry.Parse("M 2,8 L 13,8 L 17,12 L 31,12 L 31,27 L 2,27 Z"),
            Width = 34, Height = 27, Stretch = Stretch.Fill, StrokeThickness = 1.5,
            Stroke = new SolidColorBrush(Color.FromRgb(174, 181, 194)),
            Fill = new SolidColorBrush(Color.FromRgb(42, 48, 59)),
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
        });
        if (isParent)
        {
            folderIcon.Children.Add(new TextBlock
            {
                Text = "↑", FontFamily = new FontFamily("Segoe UI"), FontSize = 12,
                Foreground = new SolidColorBrush(Color.FromRgb(181, 174, 250)),
                HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(0,6,8,0)
            });
        }
        content.Children.Add(folderIcon);
        content.Children.Add(new TextBlock
        {
            Text = caption, FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(210, 214, 223)),
            TextTrimming = TextTrimming.CharacterEllipsis, TextAlignment = TextAlignment.Center
        });
        var button = new Button
        {
            Content = content, Width = 94, Padding = new Thickness(5), Margin = new Thickness(2),
            Background = Brushes.Transparent, BorderThickness = new Thickness(0), Cursor = Cursors.Hand,
            ToolTip = folder, Tag = folder
        };
        button.Click += async (_, __) => await LoadDirectoryAsync((string)button.Tag, false);
        ThumbnailPanel.Children.Add(button);
    }

    private void ClearPictureForFolder()
    {
        imageLoadCancellation?.Cancel();
        currentPath = null;
        Picture.Source = null;
        EmptyHint.Visibility = Visibility.Visible;
        TitleText.Text = "AstraView";
        StatusText.Text = "当前文件夹中没有支持的图片，可进入子文件夹继续浏览";
        ZoomText.Text = "—";
    }

    private static bool IsPathInsideFolder(string path, string folder)
    {
        return string.Equals(Path.GetDirectoryName(Path.GetFullPath(path)),
            Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
            StringComparison.OrdinalIgnoreCase);
    }

    private async Task PopulateThumbnailsAsync(IEnumerable<string> images, CancellationToken cancellationToken)
    {
        var pending = new List<Task>();
        var itemNumber = 0;
        foreach (var imagePath in images)
        {
            if (cancellationToken.IsCancellationRequested) return;
            var preview = new Image
            {
                Width = 82,
                Height = 64,
                Stretch = Stretch.Uniform,
                SnapsToDevicePixels = true
            };
            var caption = new TextBlock
            {
                Text = Path.GetFileName(imagePath),
                FontSize = 10,
                Foreground = new SolidColorBrush(Color.FromRgb(174, 179, 191)),
                TextTrimming = TextTrimming.CharacterEllipsis,
                Width = 82,
                Margin = new Thickness(0, 5, 0, 0),
                TextAlignment = TextAlignment.Center
            };
            var content = new StackPanel();
            content.Children.Add(preview);
            content.Children.Add(caption);
            var button = new Button
            {
                Content = content,
                Width = 94,
                Padding = new Thickness(5),
                Margin = new Thickness(2),
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Cursor = Cursors.Hand,
                ToolTip = imagePath,
                Tag = imagePath
            };
            button.Click += async (_, __) => await LoadImageAsync((string)button.Tag);
            ThumbnailPanel.Children.Add(button);
            pending.Add(LoadThumbnailIntoAsync(imagePath, preview, cancellationToken));
            if (++itemNumber % 50 == 0)
                await System.Windows.Threading.Dispatcher.Yield(
                    System.Windows.Threading.DispatcherPriority.Background);
        }
        try { await Task.WhenAll(pending); }
        catch (OperationCanceledException) { }
    }

    private static async Task LoadThumbnailIntoAsync(string path, Image target, CancellationToken cancellationToken)
    {
        var gate = IsHeavyThumbnail(path) ? HeavyThumbnailGate : StandardThumbnailGate;
        await gate.WaitAsync(cancellationToken);
        try
        {
            var thumbnail = await Task.Run(() => DecodeThumbnail(path), cancellationToken);
            if (!cancellationToken.IsCancellationRequested)
            {
                target.Source = thumbnail;
                _ = ThumbnailCache.StoreInBackgroundAsync(path, FolderThumbnailDecodeSize, thumbnail);
            }
        }
        catch (OperationCanceledException) { }
        catch { }
        finally { gate.Release(); }
    }

    private static bool IsHeavyThumbnail(string path)
    {
        switch (Path.GetExtension(path).ToLowerInvariant())
        {
            case ".psd": case ".psb": case ".pdf":
            case ".3fr": case ".arw": case ".cr2": case ".cr3": case ".crw":
            case ".dcr": case ".dng": case ".erf": case ".kdc": case ".mos":
            case ".mrw": case ".nef": case ".nrw": case ".orf": case ".pef":
            case ".raf": case ".raw": case ".rw2": case ".rwl": case ".sr2":
            case ".srf": case ".x3f": return true;
            default: return false;
        }
    }

    private static BitmapSource DecodeThumbnail(string path)
    {
        const int requestedSize = FolderThumbnailDecodeSize;
        var cached = ThumbnailCache.TryGet(path, requestedSize);
        if (cached != null) return cached;
        var shellCached = ShellThumbnail.TryGetFromMemoryCache(path, requestedSize);
        if (shellCached != null) return shellCached;

        var extension = Path.GetExtension(path);
        if (extension.Equals(".png", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpeg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".bmp", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".gif", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".ico", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".tif", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".tiff", StringComparison.OrdinalIgnoreCase))
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            using (var headerStream = new FileStream(path, FileMode.Open, FileAccess.Read,
                       FileShare.ReadWrite | FileShare.Delete))
            {
                var decoder = BitmapDecoder.Create(headerStream, BitmapCreateOptions.DelayCreation,
                    BitmapCacheOption.None);
                if (decoder.Frames[0].PixelWidth >= decoder.Frames[0].PixelHeight)
                    bitmap.DecodePixelWidth = requestedSize;
                else
                    bitmap.DecodePixelHeight = requestedSize;
            }
            bitmap.StreamSource = stream;
            bitmap.EndInit();
            bitmap.Freeze();
            return bitmap;
        }

        using var input = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete, 65536, FileOptions.SequentialScan);
        var decoded = ImageDecoder.DecodeThumbnailWithSize(input, requestedSize);
        var converted = BitmapSource.Create(checked((int)decoded.Width), checked((int)decoded.Height),
            96, 96, PixelFormats.Bgra32, null, decoded.Pixels, checked((int)decoded.Width * 4));
        converted.Freeze();
        return converted;
    }

    private void FitToWindow()
    {
        if (Picture.Source is not BitmapSource source || Viewport.ActualWidth <= 0) return;
        zoom = Math.Min(1, Math.Min((Viewport.ActualWidth - 24) / source.PixelWidth,
                                  (Viewport.ActualHeight - 24) / source.PixelHeight));
        ApplyZoom();
        PanTransform.X = PanTransform.Y = 0;
        CenterPicture();
    }

    private void CenterPicture()
    {
        if (Picture.Source == null || double.IsNaN(Picture.Width) || double.IsNaN(Picture.Height)) return;
        System.Windows.Controls.Canvas.SetLeft(Picture, (Viewport.ActualWidth - Picture.Width) / 2);
        System.Windows.Controls.Canvas.SetTop(Picture, (Viewport.ActualHeight - Picture.Height) / 2);
    }

    private void ApplyZoom()
    {
        zoom = Math.Max(0.02, Math.Min(32, zoom));
        ZoomTransform.ScaleX = ZoomTransform.ScaleY = zoom;
        ZoomText.Text = $"{zoom:P0}";
    }

    private static string FormatBytes(long bytes) => bytes < 1024 * 1024
        ? $"{bytes / 1024d:F1} KB" : $"{bytes / 1024d / 1024d:F1} MB";

    private async void Open_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = SupportedFormats.FileDialogFilter, CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) await LoadImageAsync(dialog.FileName);
    }
    private async void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = "选择包含图片的文件夹",
            ShowNewFolderButton = false,
            SelectedPath = currentPath == null
                ? Environment.GetFolderPath(Environment.SpecialFolder.MyPictures)
                : Path.GetDirectoryName(currentPath) ?? Environment.GetFolderPath(Environment.SpecialFolder.MyPictures)
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
            await LoadDirectoryAsync(dialog.SelectedPath ?? Environment.GetFolderPath(Environment.SpecialFolder.MyPictures));
    }

    private void RecentFolders_Click(object sender, RoutedEventArgs e)
    {
        RecentFoldersMenu.PlacementTarget = (UIElement)sender;
        RecentFoldersMenu.Placement = PlacementMode.Bottom;
        RecentFoldersMenu.IsOpen = true;
    }

    private void RecentFoldersMenu_Opened(object sender, RoutedEventArgs e)
    {
        RecentFoldersMenu.Items.Clear();
        var availableFolders = recentFolders.Where(Directory.Exists).ToList();
        if (availableFolders.Count != recentFolders.Count)
        {
            recentFolders.Clear();
            recentFolders.AddRange(availableFolders);
            SaveRecentFolders();
        }

        if (recentFolders.Count == 0)
        {
            RecentFoldersMenu.Items.Add(new MenuItem { Header = "暂无最近打开的文件夹", IsEnabled = false });
            return;
        }

        foreach (var folder in recentFolders)
        {
            var capturedFolder = folder;
            var folderName = new DirectoryInfo(folder).Name;
            var item = new MenuItem
            {
                Header = string.IsNullOrWhiteSpace(folderName) ? folder : folderName,
                ToolTip = folder
            };
            item.Click += async (_, __) => await LoadDirectoryAsync(capturedFolder);
            RecentFoldersMenu.Items.Add(item);
        }

        RecentFoldersMenu.Items.Add(new Separator { Margin = new Thickness(5,4,5,4) });
        var clearItem = new MenuItem { Header = "清除最近记录" };
        clearItem.Click += (_, __) =>
        {
            recentFolders.Clear();
            SaveRecentFolders();
        };
        RecentFoldersMenu.Items.Add(clearItem);
    }

    private async void CheckUpdates_Click(object sender, RoutedEventArgs e)
    {
        if (!UpdateButton.IsEnabled) return;
        UpdateButton.IsEnabled = false;
        var originalStatus = StatusText.Text;
        try
        {
            StatusText.Text = "正在检查更新…";
            var update = await UpdateService.CheckAsync(CancellationToken.None);
            if (update == null || !UpdateService.IsNewer(update))
            {
                StatusText.Text = $"已是最新版本 {UpdateService.CurrentVersion.ToString(3)}";
                MessageBox.Show($"当前已是最新版本 {UpdateService.CurrentVersion.ToString(3)}。",
                    "AstraView 更新", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var sizeText = update.Size > 0 ? $"\n下载大小：{update.Size / 1024d / 1024d:F1} MB" : string.Empty;
            var answer = MessageBox.Show(
                $"发现新版本 {update.Version.ToString(3)}（当前 {UpdateService.CurrentVersion.ToString(3)}）。{sizeText}\n\n现在下载并自动安装吗？",
                "AstraView 更新", MessageBoxButton.YesNo, MessageBoxImage.Information);
            if (answer != MessageBoxResult.Yes)
            {
                StatusText.Text = originalStatus;
                return;
            }

            var progress = new Progress<int>(value => StatusText.Text = $"正在下载更新… {value}%");
            var installer = await UpdateService.DownloadAsync(update, progress, CancellationToken.None);
            StatusText.Text = "下载完成，正在启动更新…";
            UpdateService.StartInstaller(installer);
            Application.Current.Shutdown();
        }
        catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            StatusText.Text = "已取消安装更新";
        }
        catch (Exception ex)
        {
            StatusText.Text = originalStatus;
            MessageBox.Show($"检查或安装更新失败：\n\n{ex.Message}",
                "AstraView 更新", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            UpdateButton.IsEnabled = true;
        }
    }

    private void LoadRecentFolders()
    {
        try
        {
            if (!File.Exists(RecentFoldersPath)) return;
            recentFolders.AddRange(File.ReadAllLines(RecentFoldersPath)
                .Where(path => !string.IsNullOrWhiteSpace(path))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .Take(8));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private void RememberFolder(string folder)
    {
        var fullPath = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        recentFolders.RemoveAll(path => string.Equals(path, fullPath, StringComparison.OrdinalIgnoreCase));
        recentFolders.Insert(0, fullPath);
        if (recentFolders.Count > 8) recentFolders.RemoveRange(8, recentFolders.Count - 8);
        SaveRecentFolders();
    }

    private void SaveRecentFolders()
    {
        try
        {
            var directory = Path.GetDirectoryName(RecentFoldersPath)!;
            Directory.CreateDirectory(directory);
            File.WriteAllLines(RecentFoldersPath, recentFolders);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
    private async void Previous_Click(object sender, RoutedEventArgs e) => await NavigateAsync(-1);
    private async void Next_Click(object sender, RoutedEventArgs e) => await NavigateAsync(1);
    private void Fit_Click(object sender, RoutedEventArgs e) => FitToWindow();
    private void Actual_Click(object sender, RoutedEventArgs e)
    {
        zoom = 1;
        ApplyZoom();
        PanTransform.X = PanTransform.Y = 0;
        CenterPicture();
    }

    private string? GetExplorerFolder()
    {
        if (!string.IsNullOrWhiteSpace(watchedFolder) && Directory.Exists(watchedFolder)) return watchedFolder;
        if (!string.IsNullOrWhiteSpace(currentPath))
        {
            var folder = Path.GetDirectoryName(currentPath);
            if (!string.IsNullOrWhiteSpace(folder) && Directory.Exists(folder)) return folder;
        }
        return null;
    }

    private static void LaunchExplorer(string arguments)
    {
        Process.Start(new ProcessStartInfo
        {
            FileName = "explorer.exe",
            Arguments = arguments,
            UseShellExecute = true
        });
    }

    private void OpenInExplorer_Click(object sender, RoutedEventArgs e)
    {
        var folder = GetExplorerFolder();
        if (folder == null)
        {
            StatusText.Text = "请先打开图片或文件夹";
            return;
        }
        LaunchExplorer($"\"{folder}\"");
    }

    private void ImageContextMenu_Opening(object sender, ContextMenuEventArgs e)
    {
        ShowInExplorerMenuItem.IsEnabled = !string.IsNullOrWhiteSpace(currentPath) && File.Exists(currentPath);
    }

    private void ShowInExplorer_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(currentPath) || !File.Exists(currentPath)) return;
        LaunchExplorer($"/select,\"{currentPath}\"");
    }
    private void Rotate_Click(object sender, RoutedEventArgs e) => RotateTransform.Angle = (RotateTransform.Angle + 90) % 360;
    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Maximize_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void Close_Click(object sender, RoutedEventArgs e) => Close();
    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2) Maximize_Click(sender, e);
        else if (e.LeftButton == MouseButtonState.Pressed) DragMove();
    }

    private void Viewport_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (Picture.Source == null) return;
        zoom *= e.Delta > 0 ? 1.15 : 1 / 1.15;
        ApplyZoom();
        e.Handled = true;
    }

    private void FolderScrollViewer_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        const double scrollStep = 96;
        var offset = FolderScrollViewer.HorizontalOffset - Math.Sign(e.Delta) * scrollStep;
        FolderScrollViewer.ScrollToHorizontalOffset(Math.Max(0, Math.Min(FolderScrollViewer.ScrollableWidth, offset)));
        e.Handled = true;
    }

    private void FolderHorizontalScrollBar_Scroll(object sender, ScrollEventArgs e) =>
        FolderScrollViewer.ScrollToHorizontalOffset(e.NewValue);

    private void FolderScrollViewer_ScrollChanged(object sender, ScrollChangedEventArgs e)
    {
        FolderHorizontalScrollBar.Maximum = Math.Max(0, e.ExtentWidth - e.ViewportWidth);
        FolderHorizontalScrollBar.ViewportSize = e.ViewportWidth;
        FolderHorizontalScrollBar.Value = Math.Max(0,
            Math.Min(FolderHorizontalScrollBar.Maximum, e.HorizontalOffset));
        FolderHorizontalScrollBar.IsEnabled = FolderHorizontalScrollBar.Maximum > 0;
    }
    private void Viewport_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        dragging = true; dragStart = e.GetPosition(Viewport); panStart = new Point(PanTransform.X, PanTransform.Y);
        Viewport.CaptureMouse(); Mouse.OverrideCursor = Cursors.Hand;
    }
    private void Viewport_MouseMove(object sender, MouseEventArgs e)
    {
        var p = e.GetPosition(Viewport);
        var inCommandZone = p.Y >= Math.Max(0, Viewport.ActualHeight - 96);
        SetFloatingCommandsVisible(inCommandZone || FloatingViewerCommands.IsMouseOver);
        if (!dragging) return;
        PanTransform.X = panStart.X + p.X - dragStart.X; PanTransform.Y = panStart.Y + p.Y - dragStart.Y;
    }
    private void Viewport_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    { dragging = false; Viewport.ReleaseMouseCapture(); Mouse.OverrideCursor = null; }
    private void Viewport_MouseLeave(object sender, MouseEventArgs e) => SetFloatingCommandsVisible(false);
    private void FloatingViewerCommands_MouseEnter(object sender, MouseEventArgs e) => SetFloatingCommandsVisible(true);
    private void FloatingViewerCommands_MouseLeave(object sender, MouseEventArgs e) => SetFloatingCommandsVisible(false);

    private void SetFloatingCommandsVisible(bool visible)
    {
        if (Picture.Source == null) visible = false;
        var targetVisibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (FloatingViewerCommands.Visibility != targetVisibility)
            FloatingViewerCommands.Visibility = targetVisibility;
    }

    private async void Window_Drop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0) await LoadImageAsync(files[0]);
    }
    private async void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Left) await NavigateAsync(-1);
        else if (e.Key == Key.Right) await NavigateAsync(1);
        else if (e.Key == Key.Escape && WindowState == WindowState.Maximized) WindowState = WindowState.Normal;
        else if (e.Key == Key.F11) WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
        else if (e.Key == Key.O && Keyboard.Modifiers == (ModifierKeys.Control | ModifierKeys.Shift)) OpenFolder_Click(sender, e);
        else if (e.Key == Key.O && Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) Open_Click(sender, e);
    }

    private static IntPtr WindowProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        const int WmGetMinMaxInfo = 0x0024;
        if (message != WmGetMinMaxInfo) return IntPtr.Zero;

        var monitor = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        if (monitor == IntPtr.Zero) return IntPtr.Zero;

        var monitorInfo = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) };
        if (!GetMonitorInfo(monitor, ref monitorInfo)) return IntPtr.Zero;

        var minMaxInfo = Marshal.PtrToStructure<MinMaxInfo>(lParam);
        var workArea = monitorInfo.WorkArea;
        var monitorArea = monitorInfo.MonitorArea;
        minMaxInfo.MaxPosition.X = Math.Abs(workArea.Left - monitorArea.Left);
        minMaxInfo.MaxPosition.Y = Math.Abs(workArea.Top - monitorArea.Top);
        minMaxInfo.MaxSize.X = Math.Abs(workArea.Right - workArea.Left);
        minMaxInfo.MaxSize.Y = Math.Abs(workArea.Bottom - workArea.Top);
        Marshal.StructureToPtr(minMaxInfo, lParam, true);
        handled = true;
        return IntPtr.Zero;
    }

    private const uint MonitorDefaultToNearest = 2;

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo monitorInfo);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MinMaxInfo
    {
        public NativePoint Reserved;
        public NativePoint MaxSize;
        public NativePoint MaxPosition;
        public NativePoint MinTrackSize;
        public NativePoint MaxTrackSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct MonitorInfo
    {
        public int Size;
        public NativeRect MonitorArea;
        public NativeRect WorkArea;
        public uint Flags;
    }
}
