#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
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

bool IsSupportedImage(const fs::path& path)
{
    static const std::vector<std::wstring> extensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif" };
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::wstring FileNameOf(const std::wstring& path)
{
    return fs::path(path).filename().wstring();
}

class ViewerWindow final
{
public:
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

        hwnd_ = CreateWindowExW(0, kClassName, L"AstraView 2.0", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance_, this);
        if (!hwnd_)
            return 1;
        DragAcceptFiles(hwnd_, TRUE);
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
        return static_cast<int>(message.wParam);
    }

    void OpenPath(const std::wstring& path)
    {
        std::error_code error;
        const fs::path candidate(path);
        if (fs::is_directory(candidate, error))
        {
            folderPath_ = candidate.wstring();
            images_.clear();
            for (const auto& entry : fs::directory_iterator(candidate, fs::directory_options::skip_permission_denied, error))
            {
                if (!error && entry.is_regular_file(error) && IsSupportedImage(entry.path()))
                    images_.push_back(entry.path().wstring());
            }
            std::sort(images_.begin(), images_.end());
            if (!images_.empty()) { currentImage_ = 0; OpenImage(images_.front()); return; }
            SetStatus(L"此文件夹没有可由 WIC 直接解码的图片");
            return;
        }
        images_.clear();
        currentImage_ = 0;
        OpenImage(path);
    }

    void OpenImage(const std::wstring& path)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
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
        }
        return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT: Paint(); return 0;
        case WM_SIZE: if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL: Zoom(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), GET_WHEEL_DELTA_WPARAM(wParam)); return 0;
        case WM_LBUTTONDOWN: dragging_ = true; lastMouse_ = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; mouseDown_ = lastMouse_; SetCapture(hwnd_); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEMOVE: Pan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONDBLCLK: FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_DROPFILES: HandleDrop(reinterpret_cast<HDROP>(wParam)); return 0;
        case WM_KEYDOWN: HandleKey(wParam); return 0;
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
        if (next != static_cast<int>(currentImage_)) { currentImage_ = static_cast<size_t>(next); OpenImage(images_[currentImage_]); }
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
        if (!wasClick || y < 0 || y > static_cast<int>(kToolbarHeight)) return;
        if (x >= 16 && x < 132) OpenFileDialog(false);
        else if (x >= 144 && x < 276) OpenFileDialog(true);
        else if (x >= 288 && x < 368) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); }
        else if (x >= 380 && x < 438) { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    void Pan(int x, int y)
    {
        if (!dragging_) return;
        panX_ += static_cast<float>(x - lastMouse_.x);
        panY_ += static_cast<float>(y - lastMouse_.y);
        lastMouse_ = { x, y };
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        const float height = static_cast<float>(client.bottom - client.top) - kToolbarHeight;
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
        panY_ = kToolbarHeight + (static_cast<float>(client.bottom) - kToolbarHeight - imageHeight_) / 2.0f;
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

    void DrawButton(const wchar_t* text, float left, float width)
    {
        const D2D1_RECT_F rect = D2D1::RectF(left, 10.0f, left + width, 46.0f);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), accentBrush_.Get(), 1.0f);
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
        DrawButton(L"打开  Ctrl+O", 16.0f, 116.0f);
        DrawButton(L"文件夹  Ctrl+L", 144.0f, 132.0f);
        DrawButton(L"适应  F", 288.0f, 80.0f);
        DrawButton(L"1:1", 380.0f, 58.0f);

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
        const std::wstring footer = status_.empty() ? L"纯 C++ / Win32 + WIC + Direct2D" : status_;
        target_->DrawTextW(footer.c_str(), static_cast<UINT32>(footer.size()), textFormat_.Get(), D2D1::RectF(16, size.height - 30, size.width - 16, size.height - 6), textBrush_.Get());
        const HRESULT result = target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
        {
            imageBitmap_.Reset(); textFormat_.Reset(); accentBrush_.Reset(); textBrush_.Reset(); toolbarBrush_.Reset(); backgroundBrush_.Reset(); target_.Reset();
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
    std::vector<std::wstring> images_;
    size_t currentImage_{};
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
