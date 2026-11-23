// Photmosp — универсальный просмотрщик фото и видео (Win32 + GDI+ + Media Foundation)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <shlobj.h>
#include <shlwapi.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;
using namespace std;

// ---------- Глобальное состояние ----------
static Image*  g_image      = nullptr;
static Bitmap* g_bitmap     = nullptr;
static std::wstring g_path;
static float   g_zoom       = 1.0f;
static float   g_fitZoom    = 1.0f;
static bool    g_autoFit    = true;
static int     g_imgW = 0, g_imgH = 0;

// Видео
static IMFSourceReader* g_pReader = nullptr;
static IMFMediaSource*  g_pSource = nullptr;
static IMFVideoDisplayControl* g_pDisplay = nullptr; // Для отрисовки
static bool g_isVideo = false;
static bool g_isPlaying = false;
static IMFSample* g_pSample = nullptr;
static LONGLONG g_videoDuration = 0;

static const wchar_t* WND_CLASS = L"PhotmospWnd";

// ---------- Утилиты ----------
static std::wstring GetFileName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

static std::wstring GetFolder(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static bool IsVideoFile(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L"."));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return (ext == L".mp4" || ext == L".avi" || ext == L".mkv" || ext == L".mov" || ext == L".wmv" || ext == L".webm");
}

static std::vector<std::wstring> ListMediaInFolder(const std::wstring& folder) {
    std::vector<std::wstring> result;
    static const wchar_t* exts[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".tif", L".tiff", L".ico",
        L".mp4", L".avi", L".mkv", L".mov", L".wmv", L".webm"
    };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        std::wstring lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        for (auto* e : exts) {
            size_t el = wcslen(e);
            if (lower.size() >= el && lower.compare(lower.size() - el, el, e) == 0) {
                result.push_back(folder + L"\\" + name);
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(result.begin(), result.end());
    return result;
}

// ---------- Инициализация Media Foundation ----------
static bool InitVideo(const std::wstring& path) {
    if (g_pReader) { g_pReader->Release(); g_pReader = nullptr; }
    if (g_pSource) { g_pSource->Release(); g_pSource = nullptr; }

    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &g_pReader))) return false;

    IMFMediaType* pType = nullptr;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    g_pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
    pType->Release();

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(g_pReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
        g_videoDuration = var.uhVal.QuadPart / 10000; // в мс
    }
    PropVariantClear(&var);

    return true;
}

// ---------- Отрисовка кадра видео ----------
static void DrawVideoFrame(HWND hwnd, HDC hdc) {
    if (!g_pReader || !g_isPlaying) return;
    
    DWORD streamIndex, flags;
    LONGLONG llTimeStamp;
    
    if (SUCCEEDED(g_pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &llTimeStamp, &g_pSample))) {
        if (g_pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            if (SUCCEEDED(g_pSample->ConvertToContiguousBuffer(&pBuffer))) {
                BYTE* pData = nullptr;
                DWORD cbData = 0;
                if (SUCCEEDED(pBuffer->Lock(&pData, nullptr, &cbData))) {
                    // Отрисовка через GDI+ (упрощенно)
                    Bitmap bmp(g_imgW, g_imgH, 4 * g_imgW, PixelFormat32bppRGB, pData);
                    Graphics gfx(hdc);
                    gfx.DrawImage(&bmp, 0, 0, g_imgW, g_imgH);
                    pBuffer->Unlock();
                }
                pBuffer->Release();
            }
            g_pSample->Release();
            g_pSample = nullptr;
        }
    }
}

// ---------- Загрузка ----------
static void ReleaseImage() {
    if (g_image)  { delete g_image;  g_image  = nullptr; }
    if (g_bitmap) { delete g_bitmap; g_bitmap = nullptr; }
}

static bool LoadMediaFile(HWND hwnd, const std::wstring& path) {
    ReleaseImage();
    if (g_pReader) { g_pReader->Release(); g_pReader = nullptr; }

    g_path = path;
    g_isVideo = IsVideoFile(path);

    if (g_isVideo) {
        if (!InitVideo(path)) {
            MessageBoxW(hwnd, L"Не удалось открыть видео", L"Photmosp", MB_ICONERROR);
            return false;
        }
        g_isPlaying = true;
        g_autoFit = true;
        SetWindowTextW(hwnd, (GetFileName(path) + L" — Photmosp").c_str());
    } else {
        g_bitmap = new Bitmap(path.c_str());
        if (g_bitmap->GetLastStatus() != Ok) {
            ReleaseImage();
            MessageBoxW(hwnd, (L"Не удалось открыть:\n" + path).c_str(), L"Photmosp", MB_ICONERROR);
            return false;
        }
        g_image  = g_bitmap;
        g_imgW   = g_bitmap->GetWidth();
        g_imgH   = g_bitmap->GetHeight();
        g_zoom   = 1.0f;
        g_autoFit = true;
        SetWindowTextW(hwnd, (GetFileName(path) + L" — Photmosp").c_str());
    }

    InvalidateRect(hwnd, nullptr, TRUE);
    return true;
}

// ---------- Диалог открытия ----------
static bool OpenFileDialog(HWND hwnd, std::wstring& outPath) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter =
        L"Все медиафайлы\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.ico;*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.webm\0"
        L"Изображения\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.ico\0"
        L"Видео\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.webm\0"
        L"Все файлы\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        outPath = buf;
        return true;
    }
    return false;
}

// ---------- Отрисовка ----------
static void PaintClient(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (!g_bitmap && !g_pReader) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(150, 150, 150));
        DrawTextW(hdc, L"Photmosp — открыть медиа (Ctrl+O)", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return;
    }

    if (g_pReader) {
        DrawVideoFrame(hwnd, hdc);
    } else if (g_bitmap) {
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        float sx = (float)cw / g_imgW;
        float sy = (float)ch / g_imgH;
        float fit = min(sx, sy);
        float z = g_autoFit ? fit : g_zoom;
        int dw = (int)(g_imgW * z);
        int dh = (int)(g_imgH * z);
        int dx = (rc.right - dw) / 2;
        int dy = (rc.bottom - dh) / 2;

        Graphics gfx(hdc);
        gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gfx.DrawImage(g_bitmap, dx, dy, dw, dh);
    }

    // Панель информации
    wchar_t info[256];
    if (g_pReader) {
        swprintf_s(info, L"%ls  [Видео]", GetFileName(g_path).c_str());
    } else {
        swprintf_s(info, L"%ls  ·  %dx%d", GetFileName(g_path).c_str(), g_imgW, g_imgH);
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(170, 170, 170));
    RECT ir = rc;
    ir.top = rc.bottom - 25;
    ir.left += 15;
    DrawTextW(hdc, info, -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    EndPaint(hwnd, &ps);
}

// ---------- Обработчик окна ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_PAINT:
        PaintClient(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (g_pReader && g_isPlaying) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        if (g_pReader) return 0; // Для видео зум не нужен
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            g_autoFit = false;
            g_zoom *= (delta > 0 ? 1.15f : 0.87f);
            g_zoom = max(0.05f, min(g_zoom, 20.0f));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wParam;
        wchar_t buf[MAX_PATH];
        DragQueryFileW(hd, 0, buf, MAX_PATH);
        DragFinish(hd);
        LoadMediaFile(hwnd, buf);
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                std::wstring p;
                if (OpenFileDialog(hwnd, p)) LoadMediaFile(hwnd, p);
            }
            break;
        case VK_SPACE:
            g_isPlaying = !g_isPlaying;
            break;
        case VK_LEFT:
        case VK_RIGHT: {
            if (g_path.empty()) break;
            auto files = ListMediaInFolder(GetFolder(g_path));
            if (files.empty()) break;
            std::wstring cur = GetFileName(g_path);
            int idx = -1;
            for (int i = 0; i < (int)files.size(); ++i)
                if (GetFileName(files[i]) == cur) { idx = i; break; }
            if (idx < 0) idx = 0;
            int n = (int)files.size();
            idx = (idx + (wParam == VK_LEFT ? -1 : 1) + n) % n;
            LoadMediaFile(hwnd, files[idx]);
            break;
        }
        case VK_ESCAPE: DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_DESTROY:
        ReleaseImage();
        if (g_pReader) g_pReader->Release();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- Точка входа ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    // Регистрация в системе как программы для медиа (для контекстного меню "Открыть с помощью")
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Photmosp.exe", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyAppName", 0, REG_SZ, (const BYTE*)L"Photmosp", sizeof(L"Photmosp"));
        RegCloseKey(hKey);
    }

    HWND hwnd = CreateWindowExW(0, WND_CLASS, L"Photmosp",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        LoadMediaFile(hwnd, argv[1]);
    }
    if (argv) LocalFree(argv);

    SetTimer(hwnd, 1, 33, nullptr); // 30 FPS для обновления видео

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(hwnd, 1);
    MFShutdown();
    GdiplusShutdown(token);
    CoUninitialize();
    return (int)msg.wParam;
}
