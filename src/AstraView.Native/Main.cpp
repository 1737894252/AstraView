#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
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
constexpr UINT kThumbnailReadyMessage = WM_APP + 19;
std::once_flag kPdfiumInitialization;

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
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif", L".pdf" };
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

class ViewerWindow final
{
public:
    ~ViewerWindow() { StopThumbnailWorker(); }

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
        StartThumbnailWorker();
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
            images_.clear();
            for (const auto& entry : fs::directory_iterator(candidate, fs::directory_options::skip_permission_denied, error))
            {
                if (!error && entry.is_regular_file(error) && IsSupportedImage(entry.path()))
                    images_.push_back(entry.path().wstring());
            }
            std::sort(images_.begin(), images_.end());
            thumbnails_.clear();
            QueueThumbnails();
            if (!images_.empty()) { currentImage_ = 0; OpenImage(images_.front()); return; }
            SetStatus(L"此文件夹没有可由 WIC 直接解码的图片");
            return;
        }
        images_.clear();
        thumbnails_.clear();
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
        case kThumbnailReadyMessage: AdoptThumbnail(reinterpret_cast<ThumbnailPixels*>(lParam)); return 0;
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
        if (!wasClick) return;
        if (HandleThumbnailClick(x, y)) return;
        if (y < 0 || y > static_cast<int>(kToolbarHeight)) return;
        if (x >= 16 && x < 132) OpenFileDialog(false);
        else if (x >= 144 && x < 276) OpenFileDialog(true);
        else if (x >= 288 && x < 368) { FitImage(); InvalidateRect(hwnd_, nullptr, FALSE); }
        else if (x >= 380 && x < 438) { SetActualSize(); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    bool HandleThumbnailClick(int x, int y)
    {
        if (images_.empty()) return false;
        RECT client{}; GetClientRect(hwnd_, &client);
        if (y < client.bottom - static_cast<int>(kThumbnailBarHeight)) return false;
        const size_t first = FirstVisibleThumbnail();
        const int slot = static_cast<int>((x - 18) / kThumbnailWidth);
        const size_t index = first + static_cast<size_t>(std::max(0, slot));
        if (slot >= 0 && index < images_.size() && x < 18 + (slot + 1) * kThumbnailWidth)
        {
            currentImage_ = index;
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
        const float height = static_cast<float>(client.bottom - client.top) - kToolbarHeight - (images_.empty() ? 0.0f : kThumbnailBarHeight);
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
        if (FAILED(factory->CreateDecoderFromFilename(task.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame))) return nullptr;
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
        if (images_.size() <= 8 || currentImage_ < 4) return 0;
        return std::min(currentImage_ - 4, images_.size() - 8);
    }

    void DrawThumbnails(const D2D1_SIZE_F& size)
    {
        if (images_.empty()) return;
        const float top = size.height - kThumbnailBarHeight;
        target_->FillRectangle(D2D1::RectF(0, top, size.width, size.height), toolbarBrush_.Get());
        const size_t first = FirstVisibleThumbnail();
        const size_t last = std::min(images_.size(), first + 8);
        for (size_t index = first; index < last; ++index)
        {
            const auto found = thumbnails_.find(index);
            const float left = 18.0f + static_cast<float>(index - first) * kThumbnailWidth;
            const auto frame = D2D1::RectF(left, top + 18.0f, left + kThumbnailWidth - 10.0f, top + 18.0f + kThumbnailHeight);
            if (index == currentImage_) target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left - 3, top + 15, left + kThumbnailWidth - 7, top + 109), 5, 5), accentBrush_.Get(), 2.0f);
            if (found != thumbnails_.end()) target_->DrawBitmap(found->second.Get(), frame, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
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
        DrawThumbnails(size);
        const std::wstring footer = status_.empty() ? L"纯 C++ / Win32 + WIC + Direct2D" : status_;
        const float footerTop = images_.empty() ? size.height - 30.0f : size.height - 22.0f;
        target_->DrawTextW(footer.c_str(), static_cast<UINT32>(footer.size()), textFormat_.Get(), D2D1::RectF(16, footerTop, size.width - 16, size.height - 4), textBrush_.Get());
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
    std::vector<std::wstring> images_;
    std::unordered_map<size_t, ComPtr<ID2D1Bitmap>> thumbnails_;
    std::thread thumbnailWorker_;
    std::mutex thumbnailMutex_;
    std::condition_variable thumbnailSignal_;
    std::deque<ThumbnailTask> thumbnailTasks_;
    std::atomic_uint thumbnailGeneration_{};
    std::atomic_bool thumbnailStopping_{};
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
