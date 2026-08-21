#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <fpdfview.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClsidText[] = L"{5E2D8E48-6F15-4C3D-AED8-BDA6544D2253}";
constexpr wchar_t kThumbnailHandler[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AstraView.ThumbnailWorker.v1";
const CLSID kClsid = {0x5e2d8e48, 0x6f15, 0x4c3d, {0xae, 0xd8, 0xbd, 0xa6, 0x54, 0x4d, 0x22, 0x53}};
HINSTANCE g_module{};
std::atomic<long> g_objects{};
std::once_flag g_pdfium;

using MagickWandHandle = void;
using MagickBoolean = int;
struct MagickApi {
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
    bool Load();
};
MagickApi g_magick;

constexpr const wchar_t* kExtensions[] = {
    L".3fr",L".arw",L".avif",L".bmp",L".cr2",L".cr3",L".crw",L".dcr",L".dds",L".dng",L".emf",L".erf",L".exr",L".gif",L".heic",L".heif",L".ico",L".jfif",L".jpe",L".jpeg",L".jpg",L".jxl",L".kdc",L".miff",L".mos",L".mrw",L".nef",L".nrw",L".orf",L".pbm",L".pcx",L".pef",L".pgm",L".png",L".pnm",L".ppm",L".psb",L".psd",L".raf",L".raw",L".rw2",L".rwl",L".sgi",L".sr2",L".srf",L".svg",L".svgz",L".tga",L".tif",L".tiff",L".webp",L".wmf",L".x3f",L".xbm",L".xpm",L".pdf"
};

HRESULT CreateBitmap(const std::vector<BYTE>& pixels, UINT width, UINT height, HBITMAP* bitmap);

bool CompleteIo(HANDLE file, OVERLAPPED& overlapped, BOOL started, DWORD& done) {
    if (started) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(overlapped.hEvent, 5000) != WAIT_OBJECT_0) { CancelIoEx(file, &overlapped); return false; }
    return GetOverlappedResult(file, &overlapped, &done, FALSE) != FALSE;
}
bool WriteExact(HANDLE file, const void* data, DWORD size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    while (size) { OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!operation.hEvent) return false;
        DWORD done{}; BOOL started = WriteFile(file, bytes, size, &done, &operation); bool ok = CompleteIo(file, operation, started, done); CloseHandle(operation.hEvent);
        if (!ok || !done) return false; bytes += done; size -= done; }
    return true;
}
bool ReadExact(HANDLE file, void* data, DWORD size) {
    auto* bytes = static_cast<BYTE*>(data);
    while (size) { OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!operation.hEvent) return false;
        DWORD done{}; BOOL started = ReadFile(file, bytes, size, &done, &operation); bool ok = CompleteIo(file, operation, started, done); CloseHandle(operation.hEvent);
        if (!ok || !done) return false; bytes += done; size -= done; }
    return true;
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_module, path, ARRAYSIZE(path));
    PathRemoveFileSpecW(path);
    return path;
}

bool MagickApi::Load() {
    if (module) return true;
    const std::wstring filename = ModuleDirectory() + L"\\CORE_RL_MagickWand_.dll";
    module = LoadLibraryW(filename.c_str());
    if (!module) {
        const std::wstring parent = ModuleDirectory() + L"\\..\\CORE_RL_MagickWand_.dll";
        module = LoadLibraryW(parent.c_str());
    }
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

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

bool IsPdfPath(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".pdf") == 0;
}

bool IsPsdPath(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && (_wcsicmp(path.c_str() + dot, L".psd") == 0 || _wcsicmp(path.c_str() + dot, L".psb") == 0);
}

HRESULT DecodeWithMagick(const std::wstring& path, UINT requestedSize, HBITMAP* bitmap) {
    if (!g_magick.Load()) return E_FAIL;
    MagickWandHandle* wand = g_magick.create();
    if (!wand) return E_OUTOFMEMORY;
    const std::string utf8 = ToUtf8(path);
    if (!g_magick.readFile(wand, utf8.c_str())) { g_magick.destroy(wand); return E_FAIL; }
    if (IsPsdPath(path) && !g_magick.setIteratorIndex(wand, 0)) { g_magick.destroy(wand); return E_FAIL; }
    const size_t originalWidth = g_magick.getWidth(wand), originalHeight = g_magick.getHeight(wand);
    if (!originalWidth || !originalHeight) { g_magick.destroy(wand); return E_FAIL; }
    const double scale = std::min(1.0, static_cast<double>(requestedSize) / std::max(originalWidth, originalHeight));
    const size_t width = std::max<size_t>(1, static_cast<size_t>(std::round(originalWidth * scale)));
    const size_t height = std::max<size_t>(1, static_cast<size_t>(std::round(originalHeight * scale)));
    if ((width != originalWidth || height != originalHeight) && !g_magick.resize(wand, width, height, 22, 1.0)) { g_magick.destroy(wand); return E_FAIL; }
    std::vector<BYTE> pixels(width * height * 4);
    const bool exported = g_magick.exportPixels(wand, 0, 0, width, height, "BGRA", 1, pixels.data()) != 0;
    g_magick.destroy(wand);
    if (!exported) return E_FAIL;
    for (size_t index = 0; index < pixels.size(); index += 4) {
        const BYTE alpha = pixels[index + 3];
        pixels[index] = static_cast<BYTE>((pixels[index] * alpha + 127) / 255);
        pixels[index + 1] = static_cast<BYTE>((pixels[index + 1] * alpha + 127) / 255);
        pixels[index + 2] = static_cast<BYTE>((pixels[index + 2] * alpha + 127) / 255);
    }
    return CreateBitmap(pixels, static_cast<UINT>(width), static_cast<UINT>(height), bitmap);
}

HRESULT DecodePdf(const std::wstring& path, UINT requestedSize, HBITMAP* bitmap) {
    std::call_once(g_pdfium, [] { FPDF_InitLibrary(); });
    const std::string utf8 = ToUtf8(path);
    FPDF_DOCUMENT document = FPDF_LoadDocument(utf8.c_str(), nullptr);
    if (!document) return E_FAIL;
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page) { FPDF_CloseDocument(document); return E_FAIL; }
    const float pageWidth = FPDF_GetPageWidthF(page), pageHeight = FPDF_GetPageHeightF(page);
    const float scale = std::min(static_cast<float>(requestedSize) / std::max(pageWidth, pageHeight), 1.0f);
    const UINT width = std::max(1u, static_cast<UINT>(std::round(pageWidth * scale)));
    const UINT height = std::max(1u, static_cast<UINT>(std::round(pageHeight * scale)));
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4, 255);
    FPDF_BITMAP pageBitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, pixels.data(), width * 4);
    if (pageBitmap) FPDF_RenderPageBitmap(pageBitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
    FPDF_ClosePage(page); FPDF_CloseDocument(document);
    if (!pageBitmap) return E_OUTOFMEMORY;
    FPDFBitmap_Destroy(pageBitmap);
    return CreateBitmap(pixels, width, height, bitmap);
}

void StartWorker() {
    std::wstring executable = ModuleDirectory() + L"\\..\\AstraView.ThumbnailWorker.exe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + executable + L"\"";
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
}

HANDLE ConnectWorker() {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    StartWorker();
    for (int attempt = 0; attempt < 20; ++attempt) {
        WaitNamedPipeW(kPipeName, 100);
        pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

HRESULT CreateBitmap(const std::vector<BYTE>& pixels, UINT width, UINT height, HBITMAP* bitmap) {
    if (!bitmap || !width || !height || pixels.size() != static_cast<size_t>(width) * height * 4) return E_INVALIDARG;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* target{};
    HBITMAP result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &target, nullptr, 0);
    if (!result || !target) return HRESULT_FROM_WIN32(GetLastError());
    memcpy(target, pixels.data(), pixels.size()); *bitmap = result; return S_OK;
}

HRESULT DecodeWithWindowsImaging(IStream* stream, UINT requestedSize, HBITMAP* bitmap) {
    if (!stream || !bitmap) return E_INVALIDARG;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICBitmapScaler* scaler{};
    IWICFormatConverter* converter{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);

    UINT sourceWidth{}, sourceHeight{};
    if (SUCCEEDED(result)) result = frame->GetSize(&sourceWidth, &sourceHeight);
    IWICBitmapSource* source = frame;
    if (SUCCEEDED(result) && (sourceWidth > requestedSize || sourceHeight > requestedSize)) {
        result = factory->CreateBitmapScaler(&scaler);
        UINT width = sourceWidth >= sourceHeight ? requestedSize :
            std::max(1u, static_cast<UINT>((static_cast<unsigned long long>(sourceWidth) * requestedSize) / sourceHeight));
        UINT height = sourceWidth >= sourceHeight ?
            std::max(1u, static_cast<UINT>((static_cast<unsigned long long>(sourceHeight) * requestedSize) / sourceWidth)) : requestedSize;
        if (SUCCEEDED(result)) result = scaler->Initialize(frame, width, height, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(result)) source = scaler;
    }
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);

    UINT width{}, height{};
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    std::vector<BYTE> pixels;
    if (SUCCEEDED(result)) {
        const UINT stride = width * 4;
        pixels.resize(static_cast<size_t>(stride) * height);
        result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(result)) result = CreateBitmap(pixels, width, height, bitmap);
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return result;
}

class ThumbnailProvider final : public IInitializeWithStream, public IInitializeWithFile, public IThumbnailProvider {
    std::atomic<ULONG> references_{1};
    IStream* stream_{};
    std::wstring path_;
public:
    ThumbnailProvider() { ++g_objects; }
    ~ThumbnailProvider() { if (stream_) stream_->Release(); --g_objects; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER; *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IInitializeWithStream)) *object = static_cast<IInitializeWithStream*>(this);
        else if (iid == __uuidof(IInitializeWithFile)) *object = static_cast<IInitializeWithFile*>(this);
        else if (iid == __uuidof(IThumbnailProvider)) *object = static_cast<IThumbnailProvider*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    IFACEMETHODIMP_(ULONG) Release() override { ULONG value = --references_; if (!value) delete this; return value; }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG; if (stream_ || !path_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream; stream_->AddRef(); return S_OK;
    }
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override {
        if (!path || stream_ || !path_.empty()) return E_INVALIDARG; path_ = path; return S_OK;
    }
    IFACEMETHODIMP GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (!bitmap || !alpha || !size) return E_INVALIDARG; *bitmap = nullptr; *alpha = WTSAT_UNKNOWN;
        if (stream_) {
            HRESULT wicResult = DecodeWithWindowsImaging(stream_, std::min(size, 2048u), bitmap);
            if (SUCCEEDED(wicResult)) { *alpha = WTSAT_ARGB; return S_OK; }
        }
        std::wstring input = path_; bool temporary = false;
        if (input.empty() && stream_) {
            STATSTG stat{};
            if (SUCCEEDED(stream_->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName) {
                if (GetFileAttributesW(stat.pwcsName) != INVALID_FILE_ATTRIBUTES) input = stat.pwcsName;
                CoTaskMemFree(stat.pwcsName);
            }
        }
        if (input.empty()) { HRESULT hr = SaveStreamToTemporaryFile(input); if (FAILED(hr)) return hr; temporary = true; }
        HRESULT result = IsPdfPath(input)
            ? DecodePdf(input, std::min(size, 2048u), bitmap)
            : DecodeWithMagick(input, std::min(size, 2048u), bitmap);
        if (temporary) DeleteFileW(input.c_str());
        if (SUCCEEDED(result)) *alpha = WTSAT_ARGB;
        return result;
    }
private:
    HRESULT SaveStreamToTemporaryFile(std::wstring& path) {
        if (!stream_) return E_UNEXPECTED;
        wchar_t folder[MAX_PATH]{}, name[MAX_PATH]{};
        if (!GetTempPathW(ARRAYSIZE(folder), folder) || !GetTempFileNameW(folder, L"AVT", 0, name)) return HRESULT_FROM_WIN32(GetLastError());
        HANDLE output = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (output == INVALID_HANDLE_VALUE) { DeleteFileW(name); return HRESULT_FROM_WIN32(GetLastError()); }
        LARGE_INTEGER zero{}; stream_->Seek(zero, STREAM_SEEK_SET, nullptr);
        BYTE buffer[64 * 1024]; HRESULT result = S_OK; bool firstChunk = true; bool isPdf = false;
        for (;;) { ULONG read{}; HRESULT hr = stream_->Read(buffer, sizeof(buffer), &read); if (FAILED(hr)) { result = hr; break; }
            if (!read) break; if (firstChunk) { isPdf = read >= 5 && memcmp(buffer, "%PDF-", 5) == 0; firstChunk = false; }
            DWORD written{}; if (!WriteFile(output, buffer, read, &written, nullptr) || written != read) { result = HRESULT_FROM_WIN32(GetLastError()); break; } }
        CloseHandle(output); if (FAILED(result)) { DeleteFileW(name); return result; }
        path = name;
        if (isPdf) { std::wstring pdfPath = path + L".pdf"; if (MoveFileExW(path.c_str(), pdfPath.c_str(), MOVEFILE_REPLACE_EXISTING)) path = pdfPath; }
        return S_OK;
    }
    HRESULT RequestThumbnail(const std::wstring& path, UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
        HANDLE pipe = ConnectWorker(); if (pipe == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
        const int chars = static_cast<int>(path.size());
        bool sent = WriteExact(pipe, &size, sizeof(size)) && WriteExact(pipe, &chars, sizeof(chars)) && WriteExact(pipe, path.data(), chars * sizeof(wchar_t));
        int status{}, bytes{}; UINT width{}, height{};
        bool received = sent && ReadExact(pipe, &status, sizeof(status)) && ReadExact(pipe, &width, sizeof(width)) && ReadExact(pipe, &height, sizeof(height)) && ReadExact(pipe, &bytes, sizeof(bytes));
        if (!received || status < 0 || bytes <= 0 || bytes > 2048 * 2048 * 4 || static_cast<unsigned long long>(width) * height * 4 != static_cast<unsigned>(bytes)) { CloseHandle(pipe); return status < 0 ? status : E_FAIL; }
        std::vector<BYTE> pixels(bytes); received = ReadExact(pipe, pixels.data(), bytes); CloseHandle(pipe);
        if (!received) return E_FAIL; HRESULT hr = CreateBitmap(pixels, width, height, bitmap); if (SUCCEEDED(hr)) *alpha = WTSAT_ARGB; return hr;
    }
};

class ClassFactory final : public IClassFactory {
    std::atomic<ULONG> references_{1};
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override { if (!object) return E_POINTER; if (iid != IID_IUnknown && iid != IID_IClassFactory) { *object = nullptr; return E_NOINTERFACE; } *object = this; AddRef(); return S_OK; }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    IFACEMETHODIMP_(ULONG) Release() override { ULONG value = --references_; if (!value) delete this; return value; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override { if (outer) return CLASS_E_NOAGGREGATION; auto* provider = new (std::nothrow) ThumbnailProvider(); if (!provider) return E_OUTOFMEMORY; HRESULT hr = provider->QueryInterface(iid, object); provider->Release(); return hr; }
    IFACEMETHODIMP LockServer(BOOL lock) override { if (lock) ++g_objects; else --g_objects; return S_OK; }
};

HRESULT SetString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY key{}; LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) { status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))); RegCloseKey(key); }
    return HRESULT_FROM_WIN32(status);
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) { g_module = module; DisableThreadLibraryCalls(module); } return TRUE; }
extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) { if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE; auto* factory = new (std::nothrow) ClassFactory(); if (!factory) return E_OUTOFMEMORY; HRESULT hr = factory->QueryInterface(iid, object); factory->Release(); return hr; }
extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t module[MAX_PATH]{}; if (!GetModuleFileNameW(g_module, module, ARRAYSIZE(module))) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = SetString(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidText + L"\\InprocServer32", nullptr, module); if (FAILED(hr)) return hr;
    SetString(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidText + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    SetString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", kClsidText, L"AstraView Thumbnail Provider");
    for (auto extension : kExtensions) { SetString(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") + extension + L"\\shellex\\" + kThumbnailHandler, nullptr, kClsidText); SetString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap", extension, L"picture"); }
    return S_OK;
}
extern "C" HRESULT __stdcall DllUnregisterServer() {
    for (auto extension : kExtensions) RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") + extension + L"\\shellex\\" + kThumbnailHandler).c_str());
    HKEY approved{}; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", 0, KEY_SET_VALUE, &approved) == ERROR_SUCCESS) { RegDeleteValueW(approved, kClsidText); RegCloseKey(approved); }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidText).c_str()); return S_OK;
}
