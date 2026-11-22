// ImageViewer.cpp — минималистичный просмотрщик изображений (Win32 + GDI+)
// Компиляция (MSVC):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE ImageViewer.cpp /link user32.lib gdi32.lib gdiplus.lib shell32.lib comdlg32.lib ole32.lib
// Или в Visual Studio: создать пустой C++ проект (Desktop), включить этот файл,
// в свойствах линкера добавить: gdiplus.lib; comdlg32.lib; shell32.lib; ole32.lib

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
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

// ---------- Глобальное состояние ----------
static Image*  g_image      = nullptr;
static Bitmap* g_bitmap     = nullptr;
static std::wstring g_path;
static float   g_zoom       = 1.0f;
static float   g_fitZoom    = 1.0f;
static bool    g_autoFit    = true;
static int     g_imgW = 0, g_imgH = 0;

static const wchar_t* WND_CLASS = L"MinimalViewerWnd";

// ---------- Утилиты ----------
static std::wstring GetFileName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

static std::wstring GetFolder(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

static std::vector<std::wstring> ListImagesInFolder(const std::wstring& folder) {
    std::vector<std::wstring> result;
    static const wchar_t* exts[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif",
        L".webp", L".tif", L".tiff", L".ico"
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

// ---------- Загрузка изображения ----------
static void ReleaseImage() {
    if (g_image)  { delete g_image;  g_image  = nullptr; }
    if (g_bitmap) { delete g_bitmap; g_bitmap = nullptr; }
}

static bool LoadImageFile(HWND hwnd, const std::wstring& path) {
    ReleaseImage();
    g_bitmap = new Bitmap(path.c_str());
    if (g_bitmap->GetLastStatus() != Ok) {
        ReleaseImage();
        MessageBoxW(hwnd, (L"Не удалось открыть:\n" + path).c_str(),
                    L"Ошибка", MB_ICONERROR);
        return false;
    }
    g_image  = g_bitmap;           // Bitmap наследует Image
    g_imgW   = g_bitmap->GetWidth();
    g_imgH   = g_bitmap->GetHeight();
    g_path   = path;
    g_zoom   = 1.0f;
    g_autoFit = true;

    std::wstring title = GetFileName(path) + L" — Minimal Viewer";
    SetWindowTextW(hwnd, title.c_str());
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
        L"Изображения\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.ico\0"
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
static void ComputeFit(HWND hwnd) {
    if (g_imgW <= 0 || g_imgH <= 0) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    float sx = (float)cw / (float)g_imgW;
    float sy = (float)ch / (float)g_imgH;
    g_fitZoom = min(sx, sy);
    if (g_fitZoom > 1.0f && g_imgW < 400 && g_imgH < 400) g_fitZoom = 1.0f; // не растягиваем мелкие
}

static void PaintClient(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(17, 17, 17));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (!g_bitmap) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(150, 150, 150));
        DrawTextW(hdc, L"Ctrl+O — открыть   •   Drag&Drop   •   ←/→ — листать",
                  -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return;
    }

    ComputeFit(hwnd);
    float z = g_autoFit ? g_fitZoom : g_zoom;
    int dw = (int)(g_imgW * z);
    int dh = (int)(g_imgH * z);
    int dx = (rc.right - dw) / 2;
    int dy = (rc.bottom - dh) / 2;

    Graphics gfx(hdc);
    gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    gfx.DrawImage(g_bitmap, dx, dy, dw, dh);

    // Инфо-строка снизу
    wchar_t info[256];
    swprintf_s(info, L"%ls  ·  %d×%d  ·  %d%%",
               GetFileName(g_path).c_str(), g_imgW, g_imgH, (int)(z * 100));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(170, 170, 170));
    RECT ir = rc;
    ir.top = rc.bottom - 22;
    ir.left += 10;
    DrawTextW(hdc, info, -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    EndPaint(hwnd, &ps);
}

// ---------- Навигация по папке ----------
static void Navigate(HWND hwnd, int dir) {
    if (g_path.empty()) return;
    auto files = ListImagesInFolder(GetFolder(g_path));
    if (files.empty()) return;
    std::wstring cur = GetFileName(g_path);
    int idx = -1;
    for (int i = 0; i < (int)files.size(); ++i)
        if (GetFileName(files[i]) == cur) { idx = i; break; }
    if (idx < 0) idx = 0;
    int n = (int)files.size();
    idx = (idx + dir + n) % n;
    LoadImageFile(hwnd, files[idx]);
}

// ---------- Обработчик окна ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_PAINT:
        PaintClient(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // фон рисуем в WM_PAINT — без мерцания

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_bitmap) return 0;
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
        LoadImageFile(hwnd, buf);
        return 0;
    }

    case WM_KEYDOWN: {
        switch (wParam) {
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                std::wstring p;
                if (OpenFileDialog(hwnd, p)) LoadImageFile(hwnd, p);
            }
            break;
        case VK_OEM_PLUS: case VK_ADD: case '=':
            if (g_bitmap) { g_autoFit = false; g_zoom = min(g_zoom * 1.25f, 20.0f);
                            InvalidateRect(hwnd, nullptr, FALSE); }
            break;
        case VK_OEM_MINUS: case VK_SUBTRACT: case '-':
            if (g_bitmap) { g_autoFit = false; g_zoom = max(g_zoom * 0.8f, 0.05f);
                            InvalidateRect(hwnd, nullptr, FALSE); }
            break;
        case '0':
            if (g_bitmap) { g_autoFit = false; g_zoom = 1.0f;
                            InvalidateRect(hwnd, nullptr, FALSE); }
            break;
        case 'F':
            if (g_bitmap) { g_autoFit = true;
                            InvalidateRect(hwnd, nullptr, FALSE); }
            break;
        case VK_LEFT:
            Navigate(hwnd, -1); break;
        case VK_RIGHT:
            Navigate(hwnd, +1); break;
        case VK_ESCAPE:
            DestroyWindow(hwnd); break;
        }
        return 0;
    }

    case WM_DESTROY:
        ReleaseImage();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- Точка входа ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // GDI+
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Регистрация класса окна
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, WND_CLASS, L"Minimal Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Открыть файл из аргумента или перетащенный на иконку
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        LoadImageFile(hwnd, argv[1]);
        LocalFree(argv);
    } else {
        // если запущено без аргумента — сразу диалог
        std::wstring p;
        if (OpenFileDialog(hwnd, p)) LoadImageFile(hwnd, p);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    GdiplusShutdown(token);
    return (int)msg.wParam;
}