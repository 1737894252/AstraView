#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <fpdfview.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
constexpr wchar_t kClassName[] = L"AstraView.Native.Window";
constexpr float kToolbarHeight = 58.0f;
constexpr float kThumbnailBarHeight = 126.0f;
constexpr float kThumbnailWidth = 120.0f;
constexpr float kThumbnailHeight = 88.0f;
constexpr float kStatusBarHeight = 44.0f;
constexpr float kResizeBorder = 6.0f;
constexpr UINT kThumbnailReadyMessage = WM_APP + 19;
constexpr UINT kDirectoryChangedMessage = WM_APP + 20;
std::once_flag kPdfiumInitialization;

using MagickWandHandle = void;
using MagickBoolean = int;
std::mutex kMagickLoadMutex;

struct MagickApi
{
    HMODULE module{};
    void (*genesis)(){};
    MagickWandHandle* (*create)(){};
    MagickWandHandle* (*destroy)(MagickWandHandle*){};
    MagickBoolean (*readFile)(MagickWandHandle*, const char*){};
    size_t (*getWidth)(MagickWandHandle*){};
    size_t (*getHeight)(MagickWandHandle*){};
    MagickBoolean (*resize)(MagickWandHandle*, size_t, size_t, int, double){};
    MagickBoolean (*exportPixels)(MagickWandHandle*, ptrdiff_t, ptrdiff_t, size_t, size_t, const char*, int, void*){};

    bool Load()
    {
        std::lock_guard lock(kMagickLoadMutex);
        if (module) return true;
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        auto directory = fs::path(path).parent_path();
        module = LoadLibraryW((directory / L"CORE_RL_MagickWand_.dll").c_str());
        if (!module) return false;
        genesis = reinterpret_cast<void (*)()>(GetProcAddress(module, "MagickWandGenesis"));
        create = reinterpret_cast<MagickWandHandle* (*)()>(GetProcAddress(module, "NewMagickWand"));
        destroy = reinterpret_cast<MagickWandHandle* (*)(MagickWandHandle*)>(GetProcAddress(module, "DestroyMagickWand"));
        readFile = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, const char*)>(GetProcAddress(module, "MagickReadImage"));
        getWidth = reinterpret_cast<size_t (*)(MagickWandHandle*)>(GetProcAddress(module, "MagickGetImageWidth"));
        getHeight = reinterpret_cast<size_t (*)(MagickWandHandle*)>(GetProcAddress(module, "MagickGetImageHeight"));
        resize = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, size_t, size_t, int, double)>(GetProcAddress(module, "MagickResizeImage"));
        exportPixels = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, ptrdiff_t, ptrdiff_t, size_t, size_t, const char*, int, void*)>(GetProcAddress(module, "MagickExportImagePixels"));
        if (!genesis || !create || !destroy || !readFile || !getWidth || !getHeight || !resize || !exportPixels) return false;
        genesis();
        return true;
    }
};

MagickApi kMagick;

void EnsurePdfiumInitialized()
{
    std::call_once(kPdfiumInitialization, [] { FPDF_InitLibrary(); });
}

struct ThumbnailTask
{
    unsigned generation{};
    size_t index{};
    std::wstring path;
};

struct ThumbnailPixels
{
    unsigned generation{};
    size_t index{};
    UINT width{}, height{};
    std::vector<BYTE> pixels;
};

bool IsSupportedImage(const fs::path& path)
{
    static const std::vector<std::wstring> extensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif", L".pdf", L".psd", L".psb", L".raw", L".dng", L".cr2", L".nef", L".arw" };
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::wstring FileNameOf(const std::wstring& path)
{
    return fs::path(path).filename().wstring();
}

bool IsPdf(const std::wstring& path)
{
    auto extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".pdf";
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

class ViewerWindow final
{
public:
    ~ViewerWindow() { StopFolderWatcher(); StopThumbnailWorker(); }

    int Run(HINSTANCE instance, int showCommand, const std::wstring& initialPath)
    {
        instance_ = instance;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
            return 1;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwriteFactory_)))
            return 1;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = instance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kClassName;
        wc.lpfnWndProc = WindowProc;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return 1;

        hwnd_ = CreateWindowExW(0, kClassName, L"AstraView 2.0", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance_, this);
        if (!hwnd_)
            return 1;
        DragAcceptFiles(hwnd_, TRUE);
        StartThumbnailWorker();
        LoadRecentFolders();
        ShowWindow(hwnd_, showCommand);
        UpdateWindow(hwnd_);
        if (!initialPath.empty())
            OpenPath(initialPath);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        StopThumbnailWorker();
        DrainPendingThumbnails();
        return static_cast<int>(message.wParam);
    }

    void OpenPath(const std::wstring& path)
    {
        std::error_code error;
        const fs::path candidate(path);
        if (fs::is_directory(candidate, error))
        {
            folderPath_ = candidate.wstring();
            RememberFolder(folderPath_);
            images_.clear();
            thumbnailOffset_ = 0;
            for (const auto& entry : fs::directory_iterator(candidate, fs::directory_options::skip_permission_denied, error))
            {
                if (!error && entry.is_regular_file(error) && IsSupportedImage(entry.path()))
                    images_.push_back(entry.path().wstring());
            }
            std::sort(images_.begin(), images_.end());
            thumbnails_.clear();
            QueueThumbnails();
            StartFolderWatcher(folderPath_);
            if (!images_.empty()) { currentImage_ = 0; OpenImage(images_.front()); return; }
            SetStatus(L"此文件夹没有可由 WIC 直接解码的图片");
            return;
        }
        images_.clear();
        thumbnails_.clear();
        thumbnailOffset_ = 0;
        StopFolderWatcher();
        CancelThumbnailTasks();
        currentImage_ = 0;
        OpenImage(path);
    }

    void OpenImage(const std::wstring& path)
    {
        if (IsPdf(path))
        {
            if (OpenPdf(path)) return;
            SetStatus(L"无法渲染 PDF：" + FileNameOf(path));
            MessageBeep(MB_ICONWARNING);
            return;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            if (OpenWithMagick(path)) return;
            SetStatus(L"无法解码：" + FileNameOf(path));
            MessageBeep(MB_ICONWARNING);
            return;
        }

        UINT width = 0, height = 0;
        frame->GetSize(&width, &height);
        ComPtr<IWICFormatConverter> converter;
        hr = wic_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            SetStatus(L"无法转换：" + FileNameOf(path));
            return;
        }

        imageSource_ = converter;
        imageWidth_ = width;
        imageHeight_ = height;
        imagePath_ = path;
        imageBitmap_.Reset();
        FitImage();
        const std::wstring position = images_.empty() ? L"" : L"  ·  " + std::to_wstring(currentImage_ + 1) + L" / " + std::to_wstring(images_.size());
        SetStatus(std::to_wstring(width) + L" × " + std::to_wstring(height) + position);
        UpdateTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<ViewerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<ViewerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: return HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        case WM_PAINT: Paint(); return 0;
        case WM_SIZE: if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL: HandleWheel(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), GET_WHEEL_DELTA_WPARAM(wParam)); return 0;
        case WM_LBUTTONDOWN: dragging_ = true; lastMouse_ = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; mouseDown_ = lastMouse_; SetCapture(hwnd_); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEMOVE: Pan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONDBLCLK: FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_DROPFILES: HandleDrop(reinterpret_cast<HDROP>(wParam)); return 0;
        case WM_KEYDOWN: HandleKey(wParam); return 0;
        case kThumbnailReadyMessage: AdoptThumbnail(reinterpret_cast<ThumbnailPixels*>(lParam)); return 0;
        case kDirectoryChangedMessage: RefreshFolder(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        return 0;
    }

    void HandleKey(WPARAM key)
    {
        if ((GetKeyState(VK_CONTROL) & 0x8000) && key == 'O') { OpenFileDialog(false); return; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && key == 'L') { OpenFileDialog(true); return; }
        if (key == 'F' || key == VK_HOME) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (key == '1') { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (key == VK_LEFT) { OpenRelativeImage(-1); return; }
        if (key == VK_RIGHT) { OpenRelativeImage(1); return; }
        if (key == VK_ESCAPE && dragging_) { dragging_ = false; ReleaseCapture(); return; }
    }

    void OpenRelativeImage(int direction)
    {
        if (images_.empty()) return;
        const int next = std::clamp(static_cast<int>(currentImage_) + direction, 0, static_cast<int>(images_.size()) - 1);
        if (next != static_cast<int>(currentImage_)) { currentImage_ = static_cast<size_t>(next); EnsureCurrentThumbnailVisible(); OpenImage(images_[currentImage_]); }
    }

    void HandleDrop(HDROP drop)
    {
        wchar_t path[MAX_PATH]{};
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) OpenPath(path);
        DragFinish(drop);
    }

    void OnMouseUp(int x, int y)
    {
        const bool wasClick = std::abs(x - mouseDown_.x) < 5 && std::abs(y - mouseDown_.y) < 5;
        dragging_ = false;
        ReleaseCapture();
        if (!wasClick) return;
        RECT client{}; GetClientRect(hwnd_, &client);
        if (y >= 0 && y < static_cast<int>(kToolbarHeight) && x >= client.right - 144)
        {
            if (x >= client.right - 48) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            else if (x >= client.right - 96) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            else ShowWindow(hwnd_, SW_MINIMIZE);
            return;
        }
        if (HandleThumbnailClick(x, y)) return;
        if (y < 0 || y > static_cast<int>(kToolbarHeight)) return;
        if (x >= 158 && x < 274) OpenFileDialog(false);
        else if (x >= 286 && x < 410) OpenFileDialog(true);
        else if (x >= 422 && x < 490) OpenRecentMenu();
        else if (x >= 502 && x < 544) OpenRelativeImage(-1);
        else if (x >= 550 && x < 592) OpenRelativeImage(1);
        else if (x >= 604 && x < 666) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); }
        else if (x >= 678 && x < 730) { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    LRESULT HitTest(int screenX, int screenY) const
    {
        POINT point{ screenX, screenY }; ScreenToClient(hwnd_, &point);
        RECT client{}; GetClientRect(hwnd_, &client);
        if (!IsZoomed(hwnd_))
        {
            const bool left = point.x < kResizeBorder, right = point.x >= client.right - kResizeBorder;
            const bool top = point.y < kResizeBorder, bottom = point.y >= client.bottom - kResizeBorder;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (point.y >= 0 && point.y < static_cast<int>(kToolbarHeight))
        {
            if ((point.x >= 158 && point.x < 274) || (point.x >= 286 && point.x < 410) || (point.x >= 422 && point.x < 490) ||
                (point.x >= 502 && point.x < 544) || (point.x >= 550 && point.x < 592) ||
                (point.x >= 604 && point.x < 666) || (point.x >= 678 && point.x < 730) || point.x >= client.right - 144)
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    bool HandleThumbnailClick(int x, int y)
    {
        if (images_.empty()) return false;
        RECT client{}; GetClientRect(hwnd_, &client);
        if (y < client.bottom - static_cast<int>(kThumbnailBarHeight + kStatusBarHeight) || y >= client.bottom - static_cast<int>(kStatusBarHeight)) return false;
        const float stripTop = static_cast<float>(client.bottom) - kStatusBarHeight - kThumbnailBarHeight;
        if (y >= stripTop + 108.0f)
        {
            const size_t visible = VisibleThumbnailCount();
            if (images_.size() > visible)
            {
                const float position = std::clamp((x - 18.0f) / std::max(1.0f, static_cast<float>(client.right) - 36.0f), 0.0f, 1.0f);
                thumbnailOffset_ = static_cast<size_t>(std::round(position * (images_.size() - visible)));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        }
        const size_t first = FirstVisibleThumbnail();
        const int slot = static_cast<int>((x - 18) / kThumbnailWidth);
        const size_t index = first + static_cast<size_t>(std::max(0, slot));
        if (slot >= 0 && index < images_.size() && x < 18 + (slot + 1) * kThumbnailWidth)
        {
            currentImage_ = index;
            EnsureCurrentThumbnailVisible();
            OpenImage(images_[currentImage_]);
        }
        return true;
    }

    void Pan(int x, int y)
    {
        if (!dragging_) return;
        panX_ += static_cast<float>(x - lastMouse_.x);
        panY_ += static_cast<float>(y - lastMouse_.y);
        lastMouse_ = { x, y };
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void HandleWheel(int screenX, int screenY, int delta)
    {
        POINT point{ screenX, screenY }; ScreenToClient(hwnd_, &point);
        RECT client{}; GetClientRect(hwnd_, &client);
        const int thumbnailTop = client.bottom - static_cast<int>(kThumbnailBarHeight + kStatusBarHeight);
        if (!images_.empty() && point.y >= thumbnailTop && point.y < client.bottom - static_cast<int>(kStatusBarHeight))
        {
            const int maximum = std::max(0, static_cast<int>(images_.size()) - static_cast<int>(VisibleThumbnailCount()));
            thumbnailOffset_ = static_cast<size_t>(std::clamp(static_cast<int>(thumbnailOffset_) + (delta > 0 ? -3 : 3), 0, maximum));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        Zoom(point.x, point.y, delta);
    }

    void Zoom(int x, int y, int delta)
    {
        if (!imageSource_) return;
        const float multiplier = delta > 0 ? 1.16f : 1.0f / 1.16f;
        const float oldScale = scale_;
        scale_ = std::clamp(scale_ * multiplier, 0.01f, 32.0f);
        const float factor = scale_ / oldScale;
        panX_ = static_cast<float>(x) - (static_cast<float>(x) - panX_) * factor;
        panY_ = static_cast<float>(y) - (static_cast<float>(y) - panY_) * factor;
        SetStatus(ZoomStatus());
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void FitImage()
    {
        if (!imageSource_ || !hwnd_) return;
        RECT client{}; GetClientRect(hwnd_, &client);
        const float width = static_cast<float>(client.right - client.left);
        const float height = static_cast<float>(client.bottom - client.top) - kToolbarHeight - kStatusBarHeight - (images_.empty() ? 0.0f : kThumbnailBarHeight);
        scale_ = std::min((width - 64.0f) / imageWidth_, (height - 64.0f) / imageHeight_);
        scale_ = std::max(scale_, 0.01f);
        panX_ = (width - imageWidth_ * scale_) / 2.0f;
        panY_ = kToolbarHeight + (height - imageHeight_ * scale_) / 2.0f;
    }

    void SetActualSize()
    {
        if (!imageSource_) return;
        RECT client{}; GetClientRect(hwnd_, &client);
        scale_ = 1.0f;
        panX_ = (static_cast<float>(client.right) - imageWidth_) / 2.0f;
        panY_ = kToolbarHeight + (static_cast<float>(client.bottom) - kToolbarHeight - kStatusBarHeight - imageHeight_) / 2.0f;
        SetStatus(ZoomStatus());
    }

    std::wstring ZoomStatus() const
    {
        return std::to_wstring(static_cast<int>(std::round(scale_ * 100))) + L"%";
    }

    void SetStatus(const std::wstring& value) { status_ = value; }
    void UpdateTitle()
    {
        const std::wstring title = imagePath_.empty() ? L"AstraView 2.0" : FileNameOf(imagePath_) + L" — AstraView 2.0";
        SetWindowTextW(hwnd_, title.c_str());
    }

    void OpenFileDialog(bool folders)
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        FILEOPENDIALOGOPTIONS options{};
        if (SUCCEEDED(dialog->GetOptions(&options)))
        {
            options |= FOS_FORCEFILESYSTEM;
            if (folders) options |= FOS_PICKFOLDERS;
            dialog->SetOptions(options);
        }
        dialog->SetTitle(folders ? L"打开图片文件夹" : L"打开图片");
        if (SUCCEEDED(dialog->Show(hwnd_)))
        {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path{};
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    OpenPath(path);
                    CoTaskMemFree(path);
                }
            }
        }
    }

    void StartFolderWatcher(const std::wstring& folder)
    {
        StopFolderWatcher();
        folderStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!folderStopEvent_) return;
        const HANDLE notification = FindFirstChangeNotificationW(folder.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (notification == INVALID_HANDLE_VALUE) { CloseHandle(folderStopEvent_); folderStopEvent_ = nullptr; return; }
        folderWatcher_ = std::thread([this, notification]
        {
            HANDLE handles[] = { folderStopEvent_, notification };
            while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
            {
                if (!FindNextChangeNotification(notification)) break;
                PostMessageW(hwnd_, kDirectoryChangedMessage, 0, 0);
            }
            FindCloseChangeNotification(notification);
        });
    }

    void StopFolderWatcher()
    {
        if (folderStopEvent_) SetEvent(folderStopEvent_);
        if (folderWatcher_.joinable()) folderWatcher_.join();
        if (folderStopEvent_) { CloseHandle(folderStopEvent_); folderStopEvent_ = nullptr; }
    }

    void RefreshFolder()
    {
        if (folderPath_.empty()) return;
        const std::wstring selected = imagePath_;
        std::vector<std::wstring> refreshed;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folderPath_, fs::directory_options::skip_permission_denied, error))
            if (!error && entry.is_regular_file(error) && IsSupportedImage(entry.path())) refreshed.push_back(entry.path().wstring());
        std::sort(refreshed.begin(), refreshed.end());
        images_ = std::move(refreshed);
        thumbnails_.clear(); thumbnailOffset_ = 0; QueueThumbnails();
        const auto found = std::find(images_.begin(), images_.end(), selected);
        if (found != images_.end()) currentImage_ = static_cast<size_t>(std::distance(images_.begin(), found));
        else if (!images_.empty()) currentImage_ = std::min(currentImage_, images_.size() - 1);
        if (!images_.empty()) { EnsureCurrentThumbnailVisible(); OpenImage(images_[currentImage_]); }
        else { imageSource_.Reset(); imageBitmap_.Reset(); imagePath_.clear(); SetStatus(L"当前文件夹没有图片"); UpdateTitle(); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    fs::path RecentFoldersFile() const
    {
        PWSTR localAppData{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) return {};
        fs::path result = fs::path(localAppData) / L"AstraView" / L"recent-folders.txt";
        CoTaskMemFree(localAppData);
        return result;
    }

    void LoadRecentFolders()
    {
        const auto file = RecentFoldersFile();
        if (file.empty()) return;
        std::ifstream input(file, std::ios::binary);
        std::string line;
        while (std::getline(input, line) && recentFolders_.size() < 12)
        {
            const auto folder = FromUtf8(line);
            std::error_code error;
            if (!folder.empty() && fs::is_directory(folder, error)) recentFolders_.push_back(folder);
        }
    }

    void RememberFolder(const std::wstring& folder)
    {
        recentFolders_.erase(std::remove(recentFolders_.begin(), recentFolders_.end(), folder), recentFolders_.end());
        recentFolders_.insert(recentFolders_.begin(), folder);
        if (recentFolders_.size() > 12) recentFolders_.resize(12);
        const auto file = RecentFoldersFile();
        if (file.empty()) return;
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        for (const auto& item : recentFolders_) output << ToUtf8(item) << '\n';
    }

    void OpenRecentMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        if (recentFolders_.empty()) AppendMenuW(menu, MF_GRAYED | MF_STRING, 0, L"暂无最近打开的文件夹");
        else
        {
            for (size_t index = 0; index < recentFolders_.size(); ++index)
                AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(1000 + index), recentFolders_[index].c_str());
        }
        POINT point{ 422, static_cast<LONG>(kToolbarHeight) }; ClientToScreen(hwnd_, &point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (command >= 1000 && command < 1000 + recentFolders_.size()) OpenPath(recentFolders_[command - 1000]);
    }

    bool OpenPdf(const std::wstring& path)
    {
        EnsurePdfiumInitialized();
        const std::string utf8Path = ToUtf8(path);
        FPDF_DOCUMENT document = FPDF_LoadDocument(utf8Path.c_str(), nullptr);
        if (!document) return false;
        FPDF_PAGE page = FPDF_LoadPage(document, 0);
        if (!page) { FPDF_CloseDocument(document); return false; }
        const float pageWidth = FPDF_GetPageWidthF(page);
        const float pageHeight = FPDF_GetPageHeightF(page);
        const float scale = std::min(2.0f, 4096.0f / std::max(pageWidth, pageHeight));
        const UINT width = std::max(1u, static_cast<UINT>(std::round(pageWidth * scale)));
        const UINT height = std::max(1u, static_cast<UINT>(std::round(pageHeight * scale)));
        pdfPixels_.assign(static_cast<size_t>(width) * height * 4, 255);
        FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(static_cast<int>(width), static_cast<int>(height), FPDFBitmap_BGRA, pdfPixels_.data(), static_cast<int>(width * 4));
        if (!bitmap) { FPDF_ClosePage(page); FPDF_CloseDocument(document); pdfPixels_.clear(); return false; }
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, static_cast<int>(width), static_cast<int>(height), 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);
        FPDF_CloseDocument(document);
        ComPtr<IWICBitmap> source;
        if (FAILED(wic_->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4, static_cast<UINT>(pdfPixels_.size()), pdfPixels_.data(), &source))) return false;
        imageSource_ = source;
        imageWidth_ = width; imageHeight_ = height; imagePath_ = path; imageBitmap_.Reset();
        FitImage();
        const std::wstring position = images_.empty() ? L"" : L"  ·  " + std::to_wstring(currentImage_ + 1) + L" / " + std::to_wstring(images_.size());
        SetStatus(L"PDF  " + std::to_wstring(width) + L" × " + std::to_wstring(height) + position);
        UpdateTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    bool OpenWithMagick(const std::wstring& path)
    {
        if (!kMagick.Load()) return false;
        MagickWandHandle* wand = kMagick.create();
        if (!wand) return false;
        const std::string utf8Path = ToUtf8(path);
        if (!kMagick.readFile(wand, utf8Path.c_str())) { kMagick.destroy(wand); return false; }
        size_t width = kMagick.getWidth(wand), height = kMagick.getHeight(wand);
        if (!width || !height) { kMagick.destroy(wand); return false; }
        constexpr size_t maximumDimension = 8192;
        if (width > maximumDimension || height > maximumDimension)
        {
            const double scale = std::min(static_cast<double>(maximumDimension) / width, static_cast<double>(maximumDimension) / height);
            width = static_cast<size_t>(width * scale); height = static_cast<size_t>(height * scale);
            if (!kMagick.resize(wand, width, height, 22, 1.0)) { kMagick.destroy(wand); return false; }
        }
        magickPixels_.resize(width * height * 4);
        const bool exported = kMagick.exportPixels(wand, 0, 0, width, height, "BGRA", 1, magickPixels_.data()) != 0;
        kMagick.destroy(wand);
        if (!exported) { magickPixels_.clear(); return false; }
        for (size_t index = 0; index < magickPixels_.size(); index += 4)
        {
            const BYTE alpha = magickPixels_[index + 3];
            magickPixels_[index] = static_cast<BYTE>((magickPixels_[index] * alpha + 127) / 255);
            magickPixels_[index + 1] = static_cast<BYTE>((magickPixels_[index + 1] * alpha + 127) / 255);
            magickPixels_[index + 2] = static_cast<BYTE>((magickPixels_[index + 2] * alpha + 127) / 255);
        }
        ComPtr<IWICBitmap> source;
        if (FAILED(wic_->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(width * 4), static_cast<UINT>(magickPixels_.size()), magickPixels_.data(), &source))) return false;
        imageSource_ = source; imageWidth_ = static_cast<UINT>(width); imageHeight_ = static_cast<UINT>(height); imagePath_ = path; imageBitmap_.Reset();
        FitImage(); SetStatus(L"ImageMagick  " + std::to_wstring(width) + L" × " + std::to_wstring(height)); UpdateTitle(); InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    void StartThumbnailWorker()
    {
        thumbnailWorker_ = std::thread([this]
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IWICImagingFactory> workerWic;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&workerWic));
            for (;;)
            {
                ThumbnailTask task;
                {
                    std::unique_lock lock(thumbnailMutex_);
                    thumbnailSignal_.wait(lock, [this] { return thumbnailStopping_ || !thumbnailTasks_.empty(); });
                    if (thumbnailStopping_) break;
                    task = std::move(thumbnailTasks_.front());
                    thumbnailTasks_.pop_front();
                }
                auto* pixels = DecodeThumbnail(workerWic.Get(), task);
                if (!pixels) continue;
                if (thumbnailStopping_ || pixels->generation != thumbnailGeneration_.load() || !PostMessageW(hwnd_, kThumbnailReadyMessage, 0, reinterpret_cast<LPARAM>(pixels)))
                    delete pixels;
            }
            CoUninitialize();
        });
    }

    void StopThumbnailWorker()
    {
        {
            std::lock_guard lock(thumbnailMutex_);
            thumbnailStopping_ = true;
            thumbnailTasks_.clear();
        }
        thumbnailSignal_.notify_all();
        if (thumbnailWorker_.joinable()) thumbnailWorker_.join();
    }

    void QueueThumbnails()
    {
        const unsigned generation = ++thumbnailGeneration_;
        std::lock_guard lock(thumbnailMutex_);
        thumbnailTasks_.clear();
        for (size_t index = 0; index < images_.size(); ++index)
            thumbnailTasks_.push_back({ generation, index, images_[index] });
        thumbnailSignal_.notify_one();
    }

    void CancelThumbnailTasks()
    {
        ++thumbnailGeneration_;
        std::lock_guard lock(thumbnailMutex_);
        thumbnailTasks_.clear();
    }

    void DrainPendingThumbnails()
    {
        MSG message{};
        while (PeekMessageW(&message, hwnd_, kThumbnailReadyMessage, kThumbnailReadyMessage, PM_REMOVE))
            delete reinterpret_cast<ThumbnailPixels*>(message.lParam);
    }

    static ThumbnailPixels* DecodeThumbnail(IWICImagingFactory* factory, const ThumbnailTask& task)
    {
        if (!factory) return nullptr;
        if (IsPdf(task.path))
        {
            EnsurePdfiumInitialized();
            const std::string utf8Path = ToUtf8(task.path);
            FPDF_DOCUMENT document = FPDF_LoadDocument(utf8Path.c_str(), nullptr);
            if (!document) return nullptr;
            FPDF_PAGE page = FPDF_LoadPage(document, 0);
            if (!page) { FPDF_CloseDocument(document); return nullptr; }
            const float widthInPoints = FPDF_GetPageWidthF(page);
            const float heightInPoints = FPDF_GetPageHeightF(page);
            const float scale = std::min(kThumbnailWidth / widthInPoints, kThumbnailHeight / heightInPoints);
            const UINT width = std::max(1u, static_cast<UINT>(widthInPoints * scale));
            const UINT height = std::max(1u, static_cast<UINT>(heightInPoints * scale));
            auto result = std::make_unique<ThumbnailPixels>();
            result->generation = task.generation; result->index = task.index; result->width = width; result->height = height;
            result->pixels.assign(static_cast<size_t>(width) * height * 4, 255);
            FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(static_cast<int>(width), static_cast<int>(height), FPDFBitmap_BGRA, result->pixels.data(), static_cast<int>(width * 4));
            if (bitmap) { FPDF_RenderPageBitmap(bitmap, page, 0, 0, static_cast<int>(width), static_cast<int>(height), 0, FPDF_ANNOT); FPDFBitmap_Destroy(bitmap); }
            FPDF_ClosePage(page); FPDF_CloseDocument(document);
            return bitmap ? result.release() : nullptr;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(factory->CreateDecoderFromFilename(task.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame)))
            return DecodeMagickThumbnail(task);
        UINT width = 0, height = 0; frame->GetSize(&width, &height);
        if (!width || !height) return nullptr;
        const float ratio = std::min(kThumbnailWidth / width, kThumbnailHeight / height);
        const UINT scaledWidth = std::max(1u, static_cast<UINT>(width * ratio));
        const UINT scaledHeight = std::max(1u, static_cast<UINT>(height * ratio));
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeFant)) ||
            FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
        auto result = std::make_unique<ThumbnailPixels>();
        result->generation = task.generation; result->index = task.index; result->width = scaledWidth; result->height = scaledHeight;
        result->pixels.resize(static_cast<size_t>(scaledWidth) * scaledHeight * 4);
        if (FAILED(converter->CopyPixels(nullptr, scaledWidth * 4, static_cast<UINT>(result->pixels.size()), result->pixels.data()))) return nullptr;
        return result.release();
    }

    static ThumbnailPixels* DecodeMagickThumbnail(const ThumbnailTask& task)
    {
        if (!kMagick.Load()) return nullptr;
        MagickWandHandle* wand = kMagick.create();
        if (!wand) return nullptr;
        const std::string utf8Path = ToUtf8(task.path);
        if (!kMagick.readFile(wand, utf8Path.c_str())) { kMagick.destroy(wand); return nullptr; }
        const size_t sourceWidth = kMagick.getWidth(wand), sourceHeight = kMagick.getHeight(wand);
        if (!sourceWidth || !sourceHeight) { kMagick.destroy(wand); return nullptr; }
        const double scale = std::min(static_cast<double>(kThumbnailWidth) / sourceWidth, static_cast<double>(kThumbnailHeight) / sourceHeight);
        const size_t width = std::max<size_t>(1, static_cast<size_t>(sourceWidth * scale));
        const size_t height = std::max<size_t>(1, static_cast<size_t>(sourceHeight * scale));
        if (!kMagick.resize(wand, width, height, 22, 1.0)) { kMagick.destroy(wand); return nullptr; }
        auto result = std::make_unique<ThumbnailPixels>();
        result->generation = task.generation; result->index = task.index; result->width = static_cast<UINT>(width); result->height = static_cast<UINT>(height);
        result->pixels.resize(width * height * 4);
        const bool exported = kMagick.exportPixels(wand, 0, 0, width, height, "BGRA", 1, result->pixels.data()) != 0;
        kMagick.destroy(wand);
        if (!exported) return nullptr;
        for (size_t index = 0; index < result->pixels.size(); index += 4)
        {
            const BYTE alpha = result->pixels[index + 3];
            result->pixels[index] = static_cast<BYTE>((result->pixels[index] * alpha + 127) / 255);
            result->pixels[index + 1] = static_cast<BYTE>((result->pixels[index + 1] * alpha + 127) / 255);
            result->pixels[index + 2] = static_cast<BYTE>((result->pixels[index + 2] * alpha + 127) / 255);
        }
        return result.release();
    }

    void AdoptThumbnail(ThumbnailPixels* pixels)
    {
        std::unique_ptr<ThumbnailPixels> owned(pixels);
        if (!pixels || pixels->generation != thumbnailGeneration_.load() || !target_) return;
        ComPtr<IWICBitmap> bitmap;
        if (SUCCEEDED(wic_->CreateBitmapFromMemory(pixels->width, pixels->height, GUID_WICPixelFormat32bppPBGRA, pixels->width * 4, static_cast<UINT>(pixels->pixels.size()), pixels->pixels.data(), &bitmap)))
        {
            ComPtr<ID2D1Bitmap> d2dBitmap;
            if (SUCCEEDED(target_->CreateBitmapFromWicBitmap(bitmap.Get(), nullptr, &d2dBitmap))) thumbnails_[pixels->index] = d2dBitmap;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool CreateDeviceResources()
    {
        if (target_) return true;
        RECT rect{}; GetClientRect(hwnd_, &rect);
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rect.right, rect.bottom)), &target_))) return false;
        if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.10f, 0.14f), &backgroundBrush_)) ||
            FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.16f, 0.22f), &toolbarBrush_)) ||
            FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0.84f, 0.88f, 0.96f), &textBrush_)) ||
            FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.65f, 1.0f), &accentBrush_))) return false;
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &textFormat_))) return false;
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return RecreateImageBitmap();
    }

    bool RecreateImageBitmap()
    {
        imageBitmap_.Reset();
        return !imageSource_ || SUCCEEDED(target_->CreateBitmapFromWicBitmap(imageSource_.Get(), nullptr, &imageBitmap_));
    }

    size_t FirstVisibleThumbnail() const
    {
        return std::min(thumbnailOffset_, images_.size() > VisibleThumbnailCount() ? images_.size() - VisibleThumbnailCount() : 0ull);
    }

    size_t VisibleThumbnailCount() const { return 8; }

    void EnsureCurrentThumbnailVisible()
    {
        const size_t visible = VisibleThumbnailCount();
        if (currentImage_ < thumbnailOffset_) thumbnailOffset_ = currentImage_;
        else if (currentImage_ >= thumbnailOffset_ + visible) thumbnailOffset_ = currentImage_ - visible + 1;
    }

    void DrawThumbnails(const D2D1_SIZE_F& size)
    {
        if (images_.empty()) return;
        const float top = size.height - kStatusBarHeight - kThumbnailBarHeight;
        target_->FillRectangle(D2D1::RectF(0, top, size.width, size.height), toolbarBrush_.Get());
        const size_t first = FirstVisibleThumbnail();
        const size_t last = std::min(images_.size(), first + VisibleThumbnailCount());
        for (size_t index = first; index < last; ++index)
        {
            const auto found = thumbnails_.find(index);
            const float left = 18.0f + static_cast<float>(index - first) * kThumbnailWidth;
            const auto frame = D2D1::RectF(left, top + 18.0f, left + kThumbnailWidth - 10.0f, top + 18.0f + kThumbnailHeight);
            if (index == currentImage_) target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left - 3, top + 15, left + kThumbnailWidth - 7, top + 109), 5, 5), accentBrush_.Get(), 2.0f);
            if (found != thumbnails_.end()) target_->DrawBitmap(found->second.Get(), frame, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        const float trackTop = top + 111.0f;
        target_->FillRectangle(D2D1::RectF(18.0f, trackTop, size.width - 18.0f, trackTop + 3.0f), backgroundBrush_.Get());
        if (images_.size() > VisibleThumbnailCount())
        {
            const float trackWidth = size.width - 36.0f;
            const float thumbWidth = std::max(34.0f, trackWidth * static_cast<float>(VisibleThumbnailCount()) / images_.size());
            const float ratio = static_cast<float>(first) / (images_.size() - VisibleThumbnailCount());
            const float left = 18.0f + (trackWidth - thumbWidth) * ratio;
            target_->FillRectangle(D2D1::RectF(left, trackTop - 1.0f, left + thumbWidth, trackTop + 4.0f), accentBrush_.Get());
        }
    }

    void DrawButton(const wchar_t* text, float left, float width)
    {
        const D2D1_RECT_F rect = D2D1::RectF(left, 10.0f, left + width, 46.0f);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), accentBrush_.Get(), 1.0f);
        target_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), textFormat_.Get(), rect, textBrush_.Get());
    }

    void DrawWindowButton(const wchar_t* text, float left, bool close)
    {
        const auto rect = D2D1::RectF(left, 0.0f, left + 48.0f, kToolbarHeight);
        if (close) target_->FillRectangle(rect, accentBrush_.Get());
        target_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), textFormat_.Get(), rect, textBrush_.Get());
    }

    void Paint()
    {
        PAINTSTRUCT ps{}; BeginPaint(hwnd_, &ps);
        if (!CreateDeviceResources()) { EndPaint(hwnd_, &ps); return; }
        const auto size = target_->GetSize();
        target_->BeginDraw();
        target_->Clear(D2D1::ColorF(0.045f, 0.055f, 0.075f));
        target_->FillRectangle(D2D1::RectF(0, 0, size.width, kToolbarHeight), toolbarBrush_.Get());
        DrawButton(L"✦  AstraView", 14.0f, 132.0f);
        DrawButton(L"打开  Ctrl+O", 158.0f, 116.0f);
        DrawButton(L"文件夹  Ctrl+L", 286.0f, 132.0f);
        DrawButton(L"最近", 422.0f, 68.0f);
        DrawButton(L"‹", 502.0f, 42.0f);
        DrawButton(L"›", 550.0f, 42.0f);
        DrawButton(L"适应", 604.0f, 62.0f);
        DrawButton(L"1:1", 678.0f, 52.0f);
        const std::wstring title = imagePath_.empty() ? L"AstraView" : FileNameOf(imagePath_);
        target_->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), textFormat_.Get(), D2D1::RectF(740, 0, std::max(740.0f, size.width - 154.0f), kToolbarHeight), textBrush_.Get());
        DrawWindowButton(L"—", size.width - 144.0f, false);
        DrawWindowButton(IsZoomed(hwnd_) ? L"▣" : L"□", size.width - 96.0f, false);
        DrawWindowButton(L"×", size.width - 48.0f, true);

        if (imageBitmap_)
        {
            const auto destination = D2D1::RectF(panX_, panY_, panX_ + imageWidth_ * scale_, panY_ + imageHeight_ * scale_);
            target_->DrawBitmap(imageBitmap_.Get(), destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            const wchar_t* prompt = L"AstraView Native Preview\n拖入图片，或按 Ctrl+O 打开";
            target_->DrawTextW(prompt, static_cast<UINT32>(wcslen(prompt)), textFormat_.Get(), D2D1::RectF(0, size.height / 2 - 25, size.width, size.height / 2 + 25), textBrush_.Get());
        }
        DrawThumbnails(size);
        const float statusTop = size.height - kStatusBarHeight;
        target_->FillRectangle(D2D1::RectF(0, statusTop, size.width, size.height), toolbarBrush_.Get());
        const std::wstring footer = status_.empty() ? L"纯 C++ / Win32 + WIC + Direct2D" : status_;
        target_->DrawTextW(footer.c_str(), static_cast<UINT32>(footer.size()), textFormat_.Get(), D2D1::RectF(16, statusTop, std::max(16.0f, size.width - 140.0f), size.height), textBrush_.Get());
        target_->DrawTextW(ZoomStatus().c_str(), static_cast<UINT32>(ZoomStatus().size()), textFormat_.Get(), D2D1::RectF(size.width - 108.0f, statusTop + 6.0f, size.width - 18.0f, size.height - 6.0f), accentBrush_.Get());
        const HRESULT result = target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
        {
            imageBitmap_.Reset(); thumbnails_.clear(); textFormat_.Reset(); accentBrush_.Reset(); textBrush_.Reset(); toolbarBrush_.Reset(); backgroundBrush_.Reset(); target_.Reset();
        }
        EndPaint(hwnd_, &ps);
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> backgroundBrush_, toolbarBrush_, textBrush_, accentBrush_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IWICBitmapSource> imageSource_;
    ComPtr<ID2D1Bitmap> imageBitmap_;
    std::wstring imagePath_, folderPath_, status_;
    std::vector<BYTE> pdfPixels_;
    std::vector<BYTE> magickPixels_;
    std::vector<std::wstring> images_;
    std::vector<std::wstring> recentFolders_;
    std::unordered_map<size_t, ComPtr<ID2D1Bitmap>> thumbnails_;
    std::thread thumbnailWorker_;
    std::mutex thumbnailMutex_;
    std::condition_variable thumbnailSignal_;
    std::deque<ThumbnailTask> thumbnailTasks_;
    std::atomic_uint thumbnailGeneration_{};
    std::atomic_bool thumbnailStopping_{};
    std::thread folderWatcher_;
    HANDLE folderStopEvent_{};
    size_t currentImage_{};
    size_t thumbnailOffset_{};
    UINT imageWidth_{}, imageHeight_{};
    float scale_{ 1.0f }, panX_{}, panY_{};
    bool dragging_{};
    POINT lastMouse_{}, mouseDown_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 1;
    std::wstring initialPath;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments && count > 1)
        initialPath = arguments[1];
    if (arguments) LocalFree(arguments);
    ViewerWindow viewer;
    const int result = viewer.Run(instance, showCommand, initialPath);
    CoUninitialize();
    return result;
}
