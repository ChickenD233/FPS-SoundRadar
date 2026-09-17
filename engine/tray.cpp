// tray.cpp - NOTIFYICON tray with bilingual menu + autostart registry.
#include "tray.h"

#include <shellapi.h>

#include <cstdio>

#include "meters.h" // g_downmixMode

namespace sr {

namespace {

constexpr UINT ID_MODE_RIGHTMONO = 1001;
constexpr UINT ID_MODE_STEREO = 1002;
constexpr UINT ID_OVERLAY = 1010;
constexpr UINT ID_CLASSIFY = 1012;
constexpr UINT ID_AUTOSTART = 1011;
constexpr UINT ID_EXIT = 1099;

const wchar_t* kTrayClass = L"SoundRadarTray";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"SoundRadar";

// Simple programmatic icon: green radar ring + center dot on transparent.
HICON CreateRadarIcon() {
    const int S = 32;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = S;
    bmi.bmiHeader.biHeight = -S; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);

    // AND mask: 1 = transparent. Start white, punch out a black disc.
    HDC maskDc = CreateCompatibleDC(screen);
    HGDIOBJ oldMask = SelectObject(maskDc, mask);
    RECT full = { 0, 0, S, S };
    FillRect(maskDc, &full, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    HGDIOBJ oldBrush = SelectObject(maskDc, black);
    HGDIOBJ oldPen = SelectObject(maskDc, GetStockObject(NULL_PEN));
    Ellipse(maskDc, 1, 1, S - 1, S - 1);
    SelectObject(maskDc, oldPen);
    SelectObject(maskDc, oldBrush);
    DeleteObject(black);
    SelectObject(maskDc, oldMask);
    DeleteDC(maskDc);

    // XOR color: black bg, green ring + center dot + 4 direction ticks.
    HGDIOBJ oldColor = SelectObject(mem, color);
    FillRect(mem, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HPEN green = CreatePen(PS_SOLID, 3, RGB(60, 255, 90));
    SelectObject(mem, green);
    SelectObject(mem, GetStockObject(NULL_BRUSH));
    Ellipse(mem, 5, 5, S - 5, S - 5);
    MoveToEx(mem, S / 2, 1, nullptr); LineTo(mem, S / 2, 6);
    MoveToEx(mem, S / 2, S - 6, nullptr); LineTo(mem, S / 2, S - 1);
    MoveToEx(mem, 1, S / 2, nullptr); LineTo(mem, 6, S / 2);
    MoveToEx(mem, S - 6, S / 2, nullptr); LineTo(mem, S - 1, S / 2);
    HBRUSH dot = CreateSolidBrush(RGB(60, 255, 90));
    SelectObject(mem, dot);
    SelectObject(mem, GetStockObject(NULL_PEN));
    Ellipse(mem, S / 2 - 3, S / 2 - 3, S / 2 + 3, S / 2 + 3);
    DeleteObject(dot);
    DeleteObject(green);
    SelectObject(mem, oldColor);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

} // namespace

bool AutostartIsEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_SZ;
}

bool AutostartSet(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool Tray::Init(const AppConfig& cfg, Handlers handlers) {
    handlers_ = std::move(handlers);
    mode_ = static_cast<int>(cfg.downmix.mode);
    overlayOn_ = cfg.overlay.enabled;
    classifyOn_ = cfg.classifyEnabled;
    autostartOn_ = AutostartIsEnabled();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    hwnd_ = CreateWindowExW(0, kTrayClass, L"SoundRadar Tray", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;

    icon_ = CreateRadarIcon();
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"SoundRadar 声纹雷达");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;

    SetTimer(hwnd_, kTimerId, 500, nullptr);
    return true;
}

void Tray::Shutdown() {
    if (hwnd_) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void Tray::Run(HANDLE quitEvent) {
    while (true) {
        DWORD r = MsgWaitForMultipleObjects(1, &quitEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Tray* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Tray*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Tray::RefreshChecks() {
    if (!menu_) return;
    CheckMenuRadioItem(modeMenu_, ID_MODE_RIGHTMONO, ID_MODE_STEREO,
                       mode_ == 0 ? ID_MODE_RIGHTMONO : ID_MODE_STEREO, MF_BYCOMMAND);
    CheckMenuItem(menu_, ID_OVERLAY, MF_BYCOMMAND | (overlayOn_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu_, ID_CLASSIFY, MF_BYCOMMAND | (classifyOn_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu_, ID_AUTOSTART,
                  MF_BYCOMMAND | (autostartOn_ ? MF_CHECKED : MF_UNCHECKED));
}

void Tray::ShowMenu() {
    if (!menu_) {
        menu_ = CreatePopupMenu();
        modeMenu_ = CreatePopupMenu();
        AppendMenuW(modeMenu_, MF_STRING, ID_MODE_RIGHTMONO, L"右耳单声道 Right-Mono");
        AppendMenuW(modeMenu_, MF_STRING, ID_MODE_STEREO, L"立体声 Stereo");
        AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu_), L"模式 Mode");
        AppendMenuW(menu_, MF_STRING, ID_OVERLAY, L"声纹显示 Overlay");
        AppendMenuW(menu_, MF_STRING, ID_CLASSIFY, L"声音分类 Sound classification (实验性)");
        AppendMenuW(menu_, MF_STRING, ID_AUTOSTART, L"开机自启 Autostart");
        AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu_, MF_STRING, ID_EXIT, L"退出 Exit");
    }
    RefreshChecks();
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_); // required so the menu dismisses properly
    TrackPopupMenu(menu_, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

LRESULT Tray::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) ShowMenu();
            return 0;
        case WM_TIMER:
            if (wp == kTimerId && handlers_.onTick) handlers_.onTick();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_MODE_RIGHTMONO:
                case ID_MODE_STEREO:
                    mode_ = (LOWORD(wp) == ID_MODE_RIGHTMONO) ? 0 : 1;
                    if (handlers_.onMode) handlers_.onMode(mode_);
                    return 0;
                case ID_OVERLAY:
                    overlayOn_ = !overlayOn_;
                    if (handlers_.onOverlay) handlers_.onOverlay(overlayOn_);
                    return 0;
                case ID_CLASSIFY:
                    classifyOn_ = !classifyOn_;
                    if (handlers_.onClassify) handlers_.onClassify(classifyOn_);
                    return 0;
                case ID_AUTOSTART:
                    autostartOn_ = !autostartOn_;
                    AutostartSet(autostartOn_);
                    if (handlers_.onAutostart) handlers_.onAutostart(autostartOn_);
                    return 0;
                case ID_EXIT:
                    if (handlers_.onExit) handlers_.onExit();
                    return 0;
                default:
                    break;
            }
            return 0;
        case WM_DESTROY:
            if (menu_) DestroyMenu(menu_);
            menu_ = nullptr;
            modeMenu_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace sr
