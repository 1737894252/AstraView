#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>
#include <d2d1.h>
#include <dwrite.h>
#include <fpdfview.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "Resource.h"

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
#pragma comment(lib, "urlmon.lib")

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
constexpr UINT kImageReadyMessage = WM_APP + 21;
constexpr UINT kUpdateReadyMessage = WM_APP + 22;
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
    MagickBoolean (*setIteratorIndex)(MagickWandHandle*, ptrdiff_t){};
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
        setIteratorIndex = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, ptrdiff_t)>(GetProcAddress(module, "MagickSetIteratorIndex"));
        getWidth = reinterpret_cast<size_t (*)(MagickWandHandle*)>(GetProcAddress(module, "MagickGetImageWidth"));
        getHeight = reinterpret_cast<size_t (*)(MagickWandHandle*)>(GetProcAddress(module, "MagickGetImageHeight"));
        resize = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, size_t, size_t, int, double)>(GetProcAddress(module, "MagickResizeImage"));
        exportPixels = reinterpret_cast<MagickBoolean (*)(MagickWandHandle*, ptrdiff_t, ptrdiff_t, size_t, size_t, const char*, int, void*)>(GetProcAddress(module, "MagickExportImagePixels"));
        if (!genesis || !create || !destroy || !readFile || !setIteratorIndex || !getWidth || !getHeight || !resize || !exportPixels) return false;
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

struct ImageTask
{
    unsigned generation{};
    std::wstring path;
    UINT maximumDimension{};
    bool highResolution{};
};

struct ImagePixels
{
    unsigned generation{};
    std::wstring path;
    UINT width{}, height{}, originalWidth{}, originalHeight{};
    bool highResolution{};
    std::vector<BYTE> pixels;
};

enum class UpdateResultKind { Check, Download };

struct UpdateResult
{
    UpdateResultKind kind{ UpdateResultKind::Check };
    bool success{};
    bool available{};
    std::wstring version;
    std::wstring downloadUrl;
    std::wstring installerPath;
    std::wstring error;
};

bool IsSupportedImage(const fs::path& path)
{
    static const std::vector<std::wstring> extensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif", L".pdf", L".psd", L".psb", L".raw", L".dng", L".cr2", L".nef", L".arw" };
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool IsWicImage(const std::wstring& path)
{
    static const std::vector<std::wstring> extensions = { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif" };
    auto extension = fs::path(path).extension().wstring();
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

bool IsPsd(const std::wstring& path)
{
    auto extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".psd" || extension == L".psb";
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
    ~ViewerWindow() { StopFolderWatcher(); StopUpdateWorker(); StopImageWorker(); StopThumbnailWorker(); }

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
        appIcon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_ASTRAVIEW));
        wc.hIcon = appIcon_;
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
        StartImageWorker();
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
        StopFolderWatcher();
        StopUpdateWorker();
        StopImageWorker();
        StopThumbnailWorker();
        DrainPendingUpdates();
        DrainPendingImages();
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
        if (IsWicImage(path))
        {
            QueueImageLoads(path);
            return;
        }
        OpenImageSynchronously(path);
    }

    void OpenImageSynchronously(const std::wstring& path)
    {
        CancelImageTasks();
        pendingImagePath_ = path;
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
        if (target_) RecreateImageBitmap();
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
        case kImageReadyMessage: AdoptImage(reinterpret_cast<ImagePixels*>(lParam)); return 0;
        case kUpdateReadyMessage: AdoptUpdate(reinterpret_cast<UpdateResult*>(lParam)); return 0;
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
        if ((GetKeyState(VK_CONTROL) & 0x8000) && key == 'U') { StartUpdateCheck(); return; }
        if (key == 'F' || key == VK_HOME) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (key == '1') { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (key == 'R') { RotateClockwise(); return; }
        if (key == VK_F11) { ToggleFullscreen(); return; }
        if (key == VK_LEFT) { OpenRelativeImage(-1); return; }
        if (key == VK_RIGHT) { OpenRelativeImage(1); return; }
        if (key == VK_ESCAPE)
        {
            if (dragging_) { dragging_ = false; ReleaseCapture(); return; }
            if (fullscreen_) { ToggleFullscreen(); return; }
        }
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
        if (x >= 58 && x < 112) OpenFileDialog(false);
        else if (x >= 118 && x < 188) OpenFileDialog(true);
        else if (x >= 194 && x < 250) OpenRecentMenu();
        else if (x >= 270 && x < 302) OpenRelativeImage(-1);
        else if (x >= 308 && x < 340) OpenRelativeImage(1);
        else if (x >= 360 && x < 406) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); }
        else if (x >= 412 && x < 456) { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); }
        else if (x >= 462 && x < 516) RotateClockwise();
        else if (x >= 522 && x < 578) ToggleFullscreen();
        else if (x >= 584 && x < 638) StartUpdateCheck();
    }

    LRESULT HitTest(int screenX, int screenY) const
    {
        POINT point{ screenX, screenY }; ScreenToClient(hwnd_, &point);
        RECT client{}; GetClientRect(hwnd_, &client);
        if (!fullscreen_ && !IsZoomed(hwnd_))
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
            if ((point.x >= 58 && point.x < 112) || (point.x >= 118 && point.x < 188) || (point.x >= 194 && point.x < 250) ||
                (point.x >= 270 && point.x < 302) || (point.x >= 308 && point.x < 340) ||
                (point.x >= 360 && point.x < 406) || (point.x >= 412 && point.x < 456) ||
                (point.x >= 462 && point.x < 516) || (point.x >= 522 && point.x < 578) ||
                (point.x >= 584 && point.x < 638) || point.x >= client.right - 144)
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
        userAdjustedView_ = true;
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
        userAdjustedView_ = true;
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
        userAdjustedView_ = false;
    }

    void SetActualSize()
    {
        if (!imageSource_) return;
        RECT client{}; GetClientRect(hwnd_, &client);
        scale_ = 1.0f;
        panX_ = (static_cast<float>(client.right) - imageWidth_) / 2.0f;
        panY_ = kToolbarHeight + (static_cast<float>(client.bottom) - kToolbarHeight - kStatusBarHeight - imageHeight_) / 2.0f;
        userAdjustedView_ = true;
        SetStatus(ZoomStatus());
    }

    void RotateClockwise()
    {
        if (!imageSource_) return;
        ComPtr<IWICBitmapFlipRotator> rotated;
        if (FAILED(wic_->CreateBitmapFlipRotator(&rotated)) || FAILED(rotated->Initialize(imageSource_.Get(), WICBitmapTransformRotate90))) return;
        imageSource_ = rotated;
        std::swap(imageWidth_, imageHeight_);
        imageBitmap_.Reset();
        if (target_) RecreateImageBitmap();
        FitImage();
        SetStatus(L"已顺时针旋转 90°  ·  " + ZoomStatus());
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ToggleFullscreen()
    {
        if (!fullscreen_)
        {
            GetWindowPlacement(hwnd_, &windowPlacement_);
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);
            SetWindowPos(hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen_ = true;
        }
        else
        {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN);
            SetWindowPlacement(hwnd_, &windowPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            fullscreen_ = false;
        }
        FitImage();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    std::wstring ZoomStatus() const
    {
        return std::to_wstring(static_cast<int>(std::round(scale_ * 100))) + L"%";
    }

    void SetStatus(const std::wstring& value) { status_ = value; }
    void UpdateTitle()
    {
        const std::wstring& activePath = pendingImagePath_.empty() ? imagePath_ : pendingImagePath_;
        const std::wstring title = activePath.empty() ? L"AstraView 2.0" : FileNameOf(activePath) + L" — AstraView 2.0";
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
        POINT point{ 194, static_cast<LONG>(kToolbarHeight) }; ClientToScreen(hwnd_, &point);
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
        imageWidth_ = width; imageHeight_ = height; imagePath_ = path; pendingImagePath_.clear(); imageBitmap_.Reset();
        if (target_) RecreateImageBitmap();
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
        if (IsPsd(path) && !kMagick.setIteratorIndex(wand, 0)) { kMagick.destroy(wand); return false; }
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
        imageSource_ = source; imageWidth_ = static_cast<UINT>(width); imageHeight_ = static_cast<UINT>(height); imagePath_ = path; pendingImagePath_.clear(); imageBitmap_.Reset();
        if (target_) RecreateImageBitmap();
        FitImage(); SetStatus(L"ImageMagick  " + std::to_wstring(width) + L" × " + std::to_wstring(height)); UpdateTitle(); InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    void StartImageWorker()
    {
        imageWorker_ = std::thread([this]
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IWICImagingFactory> workerWic;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&workerWic));
            for (;;)
            {
                ImageTask task;
                {
                    std::unique_lock lock(imageMutex_);
                    imageSignal_.wait(lock, [this] { return imageStopping_ || !imageTasks_.empty(); });
                    if (imageStopping_) break;
                    task = std::move(imageTasks_.front());
                    imageTasks_.pop_front();
                }
                auto* pixels = DecodeImage(workerWic.Get(), task);
                if (!pixels) continue;
                if (imageStopping_ || pixels->generation != imageGeneration_.load() || !PostMessageW(hwnd_, kImageReadyMessage, 0, reinterpret_cast<LPARAM>(pixels)))
                    delete pixels;
            }
            CoUninitialize();
        });
    }

    void StopImageWorker()
    {
        {
            std::lock_guard lock(imageMutex_);
            imageStopping_ = true;
            imageTasks_.clear();
        }
        imageSignal_.notify_all();
        if (imageWorker_.joinable()) imageWorker_.join();
    }

    void QueueImageLoads(const std::wstring& path)
    {
        const unsigned generation = ++imageGeneration_;
        {
            std::lock_guard lock(imageMutex_);
            imageTasks_.clear();
            imageTasks_.push_back({ generation, path, 2048, false });
            imageTasks_.push_back({ generation, path, 8192, true });
        }
        pendingImagePath_ = path;
        userAdjustedView_ = false;
        SetStatus(L"正在快速加载 " + FileNameOf(path) + L"…");
        UpdateTitle();
        imageSignal_.notify_one();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CancelImageTasks()
    {
        ++imageGeneration_;
        std::lock_guard lock(imageMutex_);
        imageTasks_.clear();
    }

    static ImagePixels* DecodeImage(IWICImagingFactory* factory, const ImageTask& task)
    {
        if (!factory) return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(factory->CreateDecoderFromFilename(task.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame))) return nullptr;
        UINT originalWidth = 0, originalHeight = 0;
        if (FAILED(frame->GetSize(&originalWidth, &originalHeight)) || !originalWidth || !originalHeight) return nullptr;
        const float ratio = std::min(1.0f, static_cast<float>(task.maximumDimension) / std::max(originalWidth, originalHeight));
        const UINT width = std::max(1u, static_cast<UINT>(std::round(originalWidth * ratio)));
        const UINT height = std::max(1u, static_cast<UINT>(std::round(originalHeight * ratio)));
        ComPtr<IWICBitmapSource> source = frame;
        ComPtr<IWICBitmapScaler> scaler;
        if (width != originalWidth || height != originalHeight)
        {
            if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return nullptr;
            source = scaler;
        }
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
        auto result = std::make_unique<ImagePixels>();
        result->generation = task.generation;
        result->path = task.path;
        result->width = width;
        result->height = height;
        result->originalWidth = originalWidth;
        result->originalHeight = originalHeight;
        result->highResolution = task.highResolution;
        result->pixels.resize(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result->pixels.size()), result->pixels.data()))) return nullptr;
        return result.release();
    }

    void DrainPendingImages()
    {
        MSG message{};
        while (PeekMessageW(&message, hwnd_, kImageReadyMessage, kImageReadyMessage, PM_REMOVE))
            delete reinterpret_cast<ImagePixels*>(message.lParam);
    }

    void AdoptImage(ImagePixels* pixels)
    {
        std::unique_ptr<ImagePixels> owned(pixels);
        if (!pixels || pixels->generation != imageGeneration_.load() || pixels->path != pendingImagePath_) return;
        ComPtr<IWICBitmap> source;
        if (FAILED(wic_->CreateBitmapFromMemory(pixels->width, pixels->height, GUID_WICPixelFormat32bppPBGRA,
            pixels->width * 4, static_cast<UINT>(pixels->pixels.size()), pixels->pixels.data(), &source))) return;

        const UINT oldWidth = imageWidth_;
        const float oldScale = scale_;
        imagePixels_ = std::move(pixels->pixels);
        imageSource_ = source;
        imageWidth_ = pixels->width;
        imageHeight_ = pixels->height;
        originalImageWidth_ = pixels->originalWidth;
        originalImageHeight_ = pixels->originalHeight;
        imagePath_ = pixels->path;
        if (pixels->highResolution) pendingImagePath_.clear();
        imageBitmap_.Reset();
        if (target_) RecreateImageBitmap();
        if (!pixels->highResolution || !userAdjustedView_) FitImage();
        else if (oldWidth) scale_ = oldScale * static_cast<float>(oldWidth) / imageWidth_;
        const std::wstring position = images_.empty() ? L"" : L"  ·  " + std::to_wstring(currentImage_ + 1) + L" / " + std::to_wstring(images_.size());
        const std::wstring quality = pixels->highResolution ? L"" : L"  ·  快速预览";
        SetStatus(std::to_wstring(originalImageWidth_) + L" × " + std::to_wstring(originalImageHeight_) + position + quality);
        UpdateTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static bool ExtractJsonString(const std::wstring& text, const wchar_t* name, std::wstring& value)
    {
        const std::wstring key = L"\"" + std::wstring(name) + L"\"";
        const size_t keyAt = text.find(key);
        if (keyAt == std::wstring::npos) return false;
        const size_t colon = text.find(L':', keyAt + key.size());
        const size_t firstQuote = colon == std::wstring::npos ? std::wstring::npos : text.find(L'\"', colon + 1);
        const size_t secondQuote = firstQuote == std::wstring::npos ? std::wstring::npos : text.find(L'\"', firstQuote + 1);
        if (secondQuote == std::wstring::npos) return false;
        value = text.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        return true;
    }

    static bool IsVersionNewer(const std::wstring& candidate, const std::wstring& current)
    {
        std::vector<int> left, right;
        auto parse = [](const std::wstring& version, std::vector<int>& values)
        {
            size_t start = 0;
            while (start < version.size() && values.size() < 4)
            {
                const size_t end = version.find(L'.', start);
                const std::wstring part = version.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                if (part.empty() || !std::all_of(part.begin(), part.end(), iswdigit)) return false;
                values.push_back(_wtoi(part.c_str()));
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            while (values.size() < 4) values.push_back(0);
            return !values.empty();
        };
        if (!parse(candidate, left) || !parse(current, right)) return false;
        return left > right;
    }

    fs::path UpdateCacheDirectory() const
    {
        PWSTR localAppData{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) return {};
        fs::path result = fs::path(localAppData) / L"AstraView" / L"Updates";
        CoTaskMemFree(localAppData);
        std::error_code error;
        fs::create_directories(result, error);
        return error ? fs::path{} : result;
    }

    UpdateResult CheckForUpdate()
    {
        UpdateResult result;
        result.kind = UpdateResultKind::Check;
        const fs::path directory = UpdateCacheDirectory();
        if (directory.empty()) { result.error = L"无法创建更新缓存目录。"; return result; }
        const fs::path manifest = directory / L"update.json";
        const HRESULT download = URLDownloadToFileW(nullptr, L"https://raw.githubusercontent.com/1737894252/AstraView/main/update.json", manifest.c_str(), 0, nullptr);
        if (FAILED(download)) { result.error = L"无法连接更新服务（" + std::to_wstring(static_cast<unsigned long>(download)) + L"）。"; return result; }
        std::ifstream input(manifest, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::wstring version;
        if (text.empty() || !ExtractJsonString(FromUtf8(text), L"version", version)) { result.error = L"更新清单格式无效。"; return result; }
        result.success = true;
        result.version = version;
        result.available = IsVersionNewer(version, L"2.0.0");
        if (result.available && !ExtractJsonString(FromUtf8(text), L"downloadUrl", result.downloadUrl))
        {
            result.success = false;
            result.error = L"更新清单缺少下载地址。";
        }
        return result;
    }

    UpdateResult DownloadUpdate(const std::wstring& version, const std::wstring& url)
    {
        UpdateResult result;
        result.kind = UpdateResultKind::Download;
        const fs::path directory = UpdateCacheDirectory();
        if (directory.empty()) { result.error = L"无法创建更新缓存目录。"; return result; }
        result.installerPath = (directory / (L"AstraView-Setup-" + version + L"-x64.exe")).wstring();
        const HRESULT download = URLDownloadToFileW(nullptr, url.c_str(), result.installerPath.c_str(), 0, nullptr);
        if (FAILED(download)) { result.error = L"下载更新失败（" + std::to_wstring(static_cast<unsigned long>(download)) + L"）。"; return result; }
        result.success = true;
        return result;
    }

    void StartUpdateCheck()
    {
        if (updateInProgress_) return;
        if (updateWorker_.joinable()) updateWorker_.join();
        updateInProgress_ = true;
        SetStatus(L"正在检查更新…");
        InvalidateRect(hwnd_, nullptr, FALSE);
        updateWorker_ = std::thread([this]
        {
            auto result = std::make_unique<UpdateResult>(CheckForUpdate());
            if (!PostMessageW(hwnd_, kUpdateReadyMessage, 0, reinterpret_cast<LPARAM>(result.get()))) return;
            result.release();
        });
    }

    void StartUpdateDownload(const std::wstring& version, const std::wstring& url)
    {
        if (updateWorker_.joinable()) updateWorker_.join();
        updateInProgress_ = true;
        SetStatus(L"正在下载更新…");
        InvalidateRect(hwnd_, nullptr, FALSE);
        updateWorker_ = std::thread([this, version, url]
        {
            auto result = std::make_unique<UpdateResult>(DownloadUpdate(version, url));
            if (!PostMessageW(hwnd_, kUpdateReadyMessage, 0, reinterpret_cast<LPARAM>(result.get()))) return;
            result.release();
        });
    }

    void StopUpdateWorker()
    {
        if (updateWorker_.joinable()) updateWorker_.join();
    }

    void DrainPendingUpdates()
    {
        MSG message{};
        while (PeekMessageW(&message, hwnd_, kUpdateReadyMessage, kUpdateReadyMessage, PM_REMOVE))
            delete reinterpret_cast<UpdateResult*>(message.lParam);
    }

    void AdoptUpdate(UpdateResult* update)
    {
        std::unique_ptr<UpdateResult> owned(update);
        if (updateWorker_.joinable()) updateWorker_.join();
        updateInProgress_ = false;
        if (!update || !update->success)
        {
            SetStatus(L"检查更新失败");
            const std::wstring error = update && !update->error.empty() ? update->error : L"更新服务不可用。";
            MessageBoxW(hwnd_, error.c_str(), L"AstraView 更新", MB_OK | MB_ICONWARNING);
            return;
        }
        if (update->kind == UpdateResultKind::Check)
        {
            if (!update->available)
            {
                SetStatus(L"已是最新版本 2.0.0");
                MessageBoxW(hwnd_, L"当前已是最新版本 2.0.0。", L"AstraView 更新", MB_OK | MB_ICONINFORMATION);
                return;
            }
            const std::wstring prompt = L"发现新版本 " + update->version + L"。现在下载并自动安装吗？";
            if (MessageBoxW(hwnd_, prompt.c_str(), L"AstraView 更新", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                StartUpdateDownload(update->version, update->downloadUrl);
            return;
        }
        SetStatus(L"下载完成，正在启动更新…");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", update->installerPath.c_str(), L"/CLOSEAPPLICATIONS", nullptr, SW_SHOWNORMAL)) <= 32)
        {
            SetStatus(L"无法启动更新安装程序");
            MessageBoxW(hwnd_, L"下载完成，但无法启动安装程序。", L"AstraView 更新", MB_OK | MB_ICONWARNING);
            return;
        }
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
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
        if (IsPsd(task.path) && !kMagick.setIteratorIndex(wand, 0)) { kMagick.destroy(wand); return nullptr; }
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
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN", &emptyStateFormat_))) return false;
        emptyStateFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        emptyStateFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        emptyStateFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
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
        const D2D1_RECT_F rect = D2D1::RectF(left, 0.0f, left + width, kToolbarHeight);
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
        DrawButton(L"打开", 58.0f, 54.0f);
        DrawButton(L"文件夹", 118.0f, 70.0f);
        DrawButton(L"最近", 194.0f, 56.0f);
        DrawButton(L"‹", 270.0f, 32.0f);
        DrawButton(L"›", 308.0f, 32.0f);
        DrawButton(L"适应", 360.0f, 46.0f);
        DrawButton(L"1:1", 412.0f, 44.0f);
        DrawButton(L"旋转", 462.0f, 54.0f);
        DrawButton(L"全屏", 522.0f, 56.0f);
        DrawButton(L"更新", 584.0f, 54.0f);
        const std::wstring& displayPath = pendingImagePath_.empty() ? imagePath_ : pendingImagePath_;
        const std::wstring title = displayPath.empty() ? L"AstraView" : FileNameOf(displayPath);
        target_->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), textFormat_.Get(), D2D1::RectF(654, 0, std::max(654.0f, size.width - 154.0f), kToolbarHeight), textBrush_.Get());
        target_->DrawLine(D2D1::Point2F(0, kToolbarHeight - 1.0f), D2D1::Point2F(size.width, kToolbarHeight - 1.0f), backgroundBrush_.Get(), 1.0f);
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
            const float contentTop = kToolbarHeight;
            const float contentBottom = size.height - kStatusBarHeight;
            const float contentCenter = (contentTop + contentBottom) / 2.0f;
            target_->DrawTextW(prompt, static_cast<UINT32>(wcslen(prompt)), emptyStateFormat_.Get(), D2D1::RectF(0, contentCenter - 38.0f, size.width, contentCenter + 38.0f), textBrush_.Get());
        }
        DrawThumbnails(size);
        const float statusTop = size.height - kStatusBarHeight;
        target_->FillRectangle(D2D1::RectF(0, statusTop, size.width, size.height), toolbarBrush_.Get());
        const std::wstring footer = status_.empty() ? L"纯 C++ / Win32 + WIC + Direct2D" : status_;
        target_->DrawTextW(footer.c_str(), static_cast<UINT32>(footer.size()), textFormat_.Get(), D2D1::RectF(16, statusTop, std::max(16.0f, size.width - 140.0f), size.height), textBrush_.Get());
        target_->DrawTextW(ZoomStatus().c_str(), static_cast<UINT32>(ZoomStatus().size()), textFormat_.Get(), D2D1::RectF(size.width - 108.0f, statusTop + 6.0f, size.width - 18.0f, size.height - 6.0f), accentBrush_.Get());
        const HRESULT result = target_->EndDraw();
        if (SUCCEEDED(result) && appIcon_)
            DrawIconEx(ps.hdc, 12, 13, appIcon_, 32, 32, 0, nullptr, DI_NORMAL);
        if (result == D2DERR_RECREATE_TARGET)
        {
            imageBitmap_.Reset(); thumbnails_.clear(); emptyStateFormat_.Reset(); textFormat_.Reset(); accentBrush_.Reset(); textBrush_.Reset(); toolbarBrush_.Reset(); backgroundBrush_.Reset(); target_.Reset();
        }
        EndPaint(hwnd_, &ps);
    }

    HINSTANCE instance_{};
    HICON appIcon_{};
    HWND hwnd_{};
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> backgroundBrush_, toolbarBrush_, textBrush_, accentBrush_;
    ComPtr<IDWriteTextFormat> textFormat_, emptyStateFormat_;
    ComPtr<IWICBitmapSource> imageSource_;
    ComPtr<ID2D1Bitmap> imageBitmap_;
    std::wstring imagePath_, pendingImagePath_, folderPath_, status_;
    std::vector<BYTE> imagePixels_;
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
    std::thread imageWorker_;
    std::mutex imageMutex_;
    std::condition_variable imageSignal_;
    std::deque<ImageTask> imageTasks_;
    std::atomic_uint imageGeneration_{};
    std::atomic_bool imageStopping_{};
    std::thread updateWorker_;
    bool updateInProgress_{};
    std::thread folderWatcher_;
    HANDLE folderStopEvent_{};
    WINDOWPLACEMENT windowPlacement_{ sizeof(WINDOWPLACEMENT) };
    size_t currentImage_{};
    size_t thumbnailOffset_{};
    UINT imageWidth_{}, imageHeight_{}, originalImageWidth_{}, originalImageHeight_{};
    float scale_{ 1.0f }, panX_{}, panY_{};
    bool dragging_{};
    bool userAdjustedView_{};
    bool fullscreen_{};
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
