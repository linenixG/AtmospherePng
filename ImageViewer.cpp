#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;
using namespace std;

static Bitmap* g_bitmap = nullptr;
static std::wstring g_path;
static bool g_autoFit = true;
static float g_zoom = 1.0f;
static int g_imgW = 0, g_imgH = 0;

static const wchar_t* WND_CLASS = L"PhotmospWnd";

static std::wstring GetFileName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

// --- Регистрация в реестре для "Открыть с помощью" ---
static void RegisterApp() {
    HKEY hKey;
    // Регистрируем прогу в системе, чтобы Windows ее видела в списке
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\Photmosp.exe", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyAppName", 0, REG_SZ, (const BYTE*)L"Photmosp", sizeof(L"Photmosp"));
        RegCloseKey(hKey);
    }
    
    // Регистрируем поддерживаемые форматы (сообщаем винде, что мы умеем открывать картинки)
    const wchar_t* exts[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp" };
    for (auto ext : exts) {
        std::wstring keyPath = L"Software\\Classes\\Photmosp" + std::wstring(ext);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
        }
    }
}

// --- Системное окно "Выбор приложения по умолчанию" ---
static void SuggestDefaultApp() {
    // Проверяем, предлагали ли мы уже это. Чтобы не бесить пользователя каждый раз.
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Photmosp", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD size = sizeof(val);
        if (RegQueryValueExW(hKey, L"AskedDefault", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS && val == 1) {
            RegCloseKey(hKey);
            return; // Уже спрашивали, больше не предлагаем
        }
        RegCloseKey(hKey);
    }

    // Создаем ключ, чтобы запомнить, что спросили
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Photmosp", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"AskedDefault", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }

    // Вызываем системное окно Windows 10/11
    IApplicationAssociationRegistrationUI* pAARUI = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pAARUI));
    if (SUCCEEDED(hr)) {
        pAARUI->LaunchAdvancedAssociationUI(L"Photmosp");
        pAARUI->Release();
    }
}

// --- Загрузка изображения ---
static void LoadImageFile(HWND hwnd, const std::wstring& path) {
    if (g_bitmap) { delete g_bitmap; g_bitmap = nullptr; }
    g_bitmap = new Bitmap(path.c_str());
    if (g_bitmap->GetLastStatus() != Ok) {
        delete g_bitmap; g_bitmap = nullptr;
        MessageBoxW(hwnd, L"Не удалось открыть файл", L"Photmosp", MB_ICONERROR);
        return;
    }
    g_imgW = g_bitmap->GetWidth();
    g_imgH = g_bitmap->GetHeight();
    g_path = path;
    g_zoom = 1.0f;
    g_autoFit = true;
    SetWindowTextW(hwnd, (GetFileName(path) + L" — Photmosp").c_str());
    InvalidateRect(hwnd, nullptr, TRUE);
}

// --- Отрисовка ---
static void PaintClient(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (!g_bitmap) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(150, 150, 150));
        DrawTextW(hdc, L"Photmosp — нажмите Ctrl+O", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return;
    }

    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    float sx = (float)cw / g_imgW;
    float sy = (float)ch / g_imgH;
    float z = g_autoFit ? min(sx, sy) : g_zoom;
    
    int dw = (int)(g_imgW * z);
    int dh = (int)(g_imgH * z);
    int dx = (rc.right - dw) / 2;
    int dy = (rc.bottom - dh) / 2;

    Graphics gfx(hdc);
    gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gfx.DrawImage(g_bitmap, dx, dy, dw, dh);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(170, 170, 170));
    RECT ir = rc; ir.top = rc.bottom - 25; ir.left += 15;
    DrawTextW(hdc, GetFileName(g_path).c_str(), -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    EndPaint(hwnd, &ps);
}

static void OpenFileDialog(HWND hwnd) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Изображения\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp\0Все файлы\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) LoadImageFile(hwnd, buf);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_PAINT: PaintClient(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) OpenFileDialog(hwnd);
        else if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_bitmap) delete g_bitmap;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    GdiplusStartupInput gsi; ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Регистрация и предложение стать по умолчанию
    RegisterApp();
    SuggestDefaultApp();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, WND_CLASS, L"Photmosp", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) LoadImageFile(hwnd, argv[1]);
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    GdiplusShutdown(token); CoUninitialize();
    return (int)msg.wParam;
}
