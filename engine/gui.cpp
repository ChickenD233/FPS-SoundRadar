// gui.cpp - main settings window, dark sci-fi theme. Plain Win32 + common
// controls; no third-party UI libs. Chinese-primary bilingual labels.
#include "gui.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <cstdio>
#include <map>

#include "log.h"
#include "meters.h" // g_downmix / g_overlay / g_analysis / g_classifyEnabled
#include "tray.h"   // AutostartIsEnabled/AutostartSet
#include "wasapi_util.h"

#pragma comment(lib, "comctl32")
#pragma comment(lib, "uxtheme")

namespace sr {

const wchar_t* kGuiClassName = L"SoundRadarMainWnd";

namespace {

constexpr UINT kStatusTimer = 42;

// theme colors (COLORREF is 0x00BBGGRR)
constexpr COLORREF kBg = RGB(0x14, 0x17, 0x1f);       // dark charcoal/navy
constexpr COLORREF kPanel = RGB(0x1c, 0x21, 0x30);    // control fill
constexpr COLORREF kPanelHot = RGB(0x23, 0x2a, 0x3d); // hover fill
constexpr COLORREF kText = RGB(0xd8, 0xdc, 0xe6);     // light gray text
constexpr COLORREF kAccent = RGB(0x00, 0xd2, 0xc8);   // cyan accent (#00d2c8)
constexpr COLORREF kDanger = RGB(0xe0, 0x60, 0x60);   // exit button
constexpr COLORREF kSep = RGB(0x2a, 0x30, 0x40);      // separator line
constexpr COLORREF kStrip = RGB(0x10, 0x14, 0x1d);    // status strip

// slider ranges
constexpr int kFadeMin = 300, kFadeMax = 500;
constexpr int kRadiusMin = 60, kRadiusMax = 160;
constexpr int kThrMin = 5, kThrMax = 95;     // percent
constexpr int kPosXMin = -50, kPosXMax = 50; // percent of screen
constexpr int kPosYMin = 0, kPosYMax = 50;
constexpr int kFxMin = 0, kFxMax = 100;
constexpr int kWMax = 30;                    // weight * 20

const wchar_t* kWeightNames[8] = { L"FL", L"FR", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR" };
const float kDefaultWeights[8] = { 0.7f, 1.0f, 1.0f, 0.7f, 0.8f, 0.8f, 0.9f, 0.9f };

bool HiddenTestMode() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"SOUNDRADAR_TEST_HIDE", buf, 8);
        cached = (n > 0 && buf[0] == L'1') ? 1 : 0;
    }
    return cached == 1;
}

} // namespace

// --- control construction helpers --------------------------------------------

HWND Gui::Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
             int w, int h, int id, DWORD ex) {
    HWND c = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    return c;
}

void Gui::Caption(const wchar_t* text, int x, int y, int w) {
    HWND cap = Mk(L"STATIC", text, SS_LEFT, x, y, 300, 18, -1);
    accentStatics_.push_back(cap);
    if (titleFont_) SendMessageW(cap, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    HWND sep = Mk(L"STATIC", L"", SS_LEFT, x, y + 22, w, 1, -1);
    sepStatics_.push_back(sep);
}

HWND Gui::Slider(int x, int y, int w, int id, int mn, int mx) {
    HWND s = Mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, x, y, w, 22, id);
    SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(mn, mx));
    SendMessageW(s, TBM_SETPAGESIZE, 0, (mx - mn) / 10 + 1);
    SetWindowTheme(s, L"DarkMode_Explorer", nullptr);
    return s;
}

HWND Gui::FlatButton(const wchar_t* text, int x, int y, int w, int id) {
    HWND b = Mk(L"BUTTON", text, BS_OWNERDRAW, x, y, w, 26, id);
    ownerBtns_.push_back(b);
    btnHover_[b] = false;
    SetWindowSubclass(b, BtnSubProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return b;
}

LRESULT CALLBACK Gui::BtnSubProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR refData) {
    Gui* self = reinterpret_cast<Gui*>(refData);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!self->btnHover_[hwnd]) {
                self->btnHover_[hwnd] = true;
                InvalidateRect(hwnd, nullptr, TRUE);
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            self->btnHover_[hwnd] = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void Gui::DrawButton(LPDRAWITEMSTRUCT dis) {
    bool hover = btnHover_[dis->hwndItem];
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool isExit = (dis->CtlID == IDC_BTN_EXIT);
    COLORREF border = isExit ? kDanger : kAccent;
    COLORREF fill = pressed ? kStrip : (hover ? kPanelHot : kPanel);

    HBRUSH b = CreateSolidBrush(fill);
    FillRect(dis->hDC, &dis->rcItem, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right,
              dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    wchar_t text[64] = {};
    GetWindowTextW(dis->hwndItem, text, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, isExit ? RGB(0xe0, 0x80, 0x80) : kText);
    if (font_) {
        HGDIOBJ oldFont = SelectObject(dis->hDC, font_);
        DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldFont);
    } else {
        DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void Gui::DrawStatusDot(LPDRAWITEMSTRUCT dis) {
    COLORREF col = statusLevel_ == 0 ? RGB(0x3f, 0xd9, 0x7c)   // running: green
                   : statusLevel_ == 1 ? RGB(0xe8, 0xb9, 0x3f)  // waiting: amber
                                       : RGB(0xe0, 0x50, 0x50); // error: red
    HBRUSH b = CreateSolidBrush(kStrip);
    FillRect(dis->hDC, &dis->rcItem, b);
    DeleteObject(b);
    HBRUSH dot = CreateSolidBrush(col);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, dot);
    HGDIOBJ oldPen = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
    Ellipse(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right,
            dis->rcItem.bottom);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(dot);
}

// --- window ------------------------------------------------------------------

bool Gui::Create(const Hooks& hooks, bool hidden) {
    hooks_ = hooks;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc); // trackbars

    bgBrush_ = CreateSolidBrush(kBg);
    stripBrush_ = CreateSolidBrush(kStrip);
    sepBrush_ = CreateSolidBrush(kSep);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kGuiClassName;
    wc.hbrBackground = bgBrush_;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("gui: RegisterClassEx failed err=%lu", GetLastError());
        return false;
    }

    RECT rc = { 0, 0, 520, 752 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    hwnd_ = CreateWindowExW(0, kGuiClassName, L"SoundRadar 声纹雷达",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log("gui: CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }

    font_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Microsoft YaHei UI");
    titleFont_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    BuildControls();
    RefreshDevices();
    LoadFromConfig();
    appliedInput_ = hooks_.cfg->captureDevice;
    appliedOutput_ = hooks_.cfg->outputDevice;

    std::vector<HWND> kids;
    struct Ctx { std::vector<HWND>* list; } ctx{ &kids };
    EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM lp) -> BOOL {
            reinterpret_cast<Ctx*>(lp)->list->push_back(child);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    for (HWND k : kids)
        SendMessageW(k, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    // re-apply bigger font to title + captions (after the blanket pass)
    for (HWND c : accentStatics_)
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);

    SetTimer(hwnd_, kStatusTimer, 500, nullptr);
    if (!hidden) Show();
    Log("gui: window created (hidden=%d)", hidden ? 1 : 0);
    return true;
}

void Gui::Show() {
    if (!hwnd_) return;
    if (HiddenTestMode()) { // headless verification: never show
        Log("gui: activate requested (hidden test mode)");
        return;
    }
    ShowWindow(hwnd_, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myT = GetCurrentThreadId();
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
    SetForegroundWindow(hwnd_);
    BringWindowToTop(hwnd_);
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
    RefreshDevices();
}

void Gui::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool Gui::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

LRESULT CALLBACK Gui::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gui* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Gui*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd; // needed before CreateWindowEx returns
    } else {
        self = reinterpret_cast<Gui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Gui::BuildControls() {
    // title strip
    HWND title = Mk(L"STATIC", L"SoundRadar 声纹雷达", SS_LEFT, 16, 10, 300, 24, -1);
    accentStatics_.push_back(title);
    HWND titleSub = Mk(L"STATIC", L"声道方向声纹 · v0.6", SS_LEFT, 320, 14, 180, 16, -1);
    dimStatics_.push_back(titleSub);

    // 设备 Devices
    Caption(L"设备 DEVICES", 16, 48, 488);
    Mk(L"STATIC", L"输入/捕获 Input", SS_LEFT, 22, 86, 96, 18, -1);
    Mk(L"STATIC", L"输出 Output", SS_LEFT, 22, 118, 96, 18, -1);
    HWND ci = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 122, 82, 382, 300,
                 IDC_COMBO_INPUT);
    HWND co = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 122, 114, 382, 300,
                 IDC_COMBO_OUTPUT);
    SetWindowTheme(ci, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(co, L"DarkMode_Explorer", nullptr);

    // 模式 Mode
    Caption(L"模式 MODE", 16, 150, 488);
    Mk(L"BUTTON", L"右耳单声道 Right-Mono", BS_AUTORADIOBUTTON | WS_GROUP, 22, 182, 180,
       20, IDC_RADIO_MONO);
    Mk(L"BUTTON", L"立体声 Stereo", BS_AUTORADIOBUTTON, 210, 182, 150, 20,
       IDC_RADIO_STEREO);

    // 声纹样式 Overlay style
    Caption(L"声纹样式 OVERLAY STYLE", 16, 214, 488);
    Mk(L"BUTTON", L"显示声纹 Show overlay", BS_AUTOCHECKBOX, 22, 246, 180, 20,
       IDC_CHK_OVERLAY);
    struct Row { int id; const wchar_t* label; int mn, mx; };
    const Row rows[] = {
        { IDC_SLIDER_FADE, L"渐隐时间 Fade", kFadeMin, kFadeMax },
        { IDC_SLIDER_RADIUS, L"雷达大小 Size", kRadiusMin, kRadiusMax },
        { IDC_SLIDER_LOW, L"阈值 绿→黄 Low", kThrMin, kThrMax },
        { IDC_SLIDER_HIGH, L"阈值 黄→红 High", kThrMin, kThrMax },
        { IDC_SLIDER_POSX, L"位置X Pos X", kPosXMin, kPosXMax },
        { IDC_SLIDER_POSY, L"位置Y Pos Y", kPosYMin, kPosYMax },
        { IDC_SLIDER_FX, L"特效强度 FX", kFxMin, kFxMax },
    };
    int y = 274;
    for (const Row& r : rows) {
        Mk(L"STATIC", r.label, SS_LEFT, 22, y + 2, 96, 18, -1);
        Slider(122, y, 290, r.id, r.mn, r.mx);
        Mk(L"STATIC", L"", SS_LEFT, 418, y + 2, 80, 18, r.id + 500); // value label
        y += 28;
    }

    // 8 声道权重 Weights
    Caption(L"8 声道权重 WEIGHTS", 16, 478, 488);
    FlatButton(L"重置 Reset", 425, 476, 75, IDC_BTN_RESETW);
    for (int i = 0; i < 8; ++i) {
        int col = i % 4, row = i / 4;
        int x = 22 + col * 122;
        int ly = 506 + row * 56;
        HWND lb = Mk(L"STATIC", kWeightNames[i], SS_LEFT, x, ly, 40, 14, -1);
        dimStatics_.push_back(lb);
        HWND s = Slider(x, ly + 14, 108, IDC_SLIDER_W0 + i, 0, kWMax);
        SendMessageW(s, TBM_SETTICFREQ, 5, 0);
    }

    // 其他 Other
    Caption(L"其他 MISC", 16, 596, 488);
    Mk(L"BUTTON", L"声音分类 (实验性)", BS_AUTOCHECKBOX, 22, 628, 200, 20,
       IDC_CHK_CLASSIFY);
    Mk(L"BUTTON", L"开机自启 Autostart", BS_AUTOCHECKBOX, 240, 628, 160, 20,
       IDC_CHK_AUTOSTART);

    // buttons + status strip
    FlatButton(L"退出程序 Exit", 180, 666, 100, IDC_BTN_EXIT);
    FlatButton(L"应用 Apply", 290, 666, 100, IDC_BTN_APPLY);
    FlatButton(L"确定 OK", 400, 666, 100, IDC_BTN_OK);
    stripBg_ = Mk(L"STATIC", L"", SS_LEFT, 0, 702, 520, 50, -1); // status strip bg
    Mk(L"STATIC", L"", SS_OWNERDRAW, 18, 712, 12, 12, IDC_STATUSDOT);
    Mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 40, 706, 468, 42, IDC_STATUS);
}

void Gui::RefreshDevices() {
    HWND in = GetDlgItem(hwnd_, IDC_COMBO_INPUT);
    HWND out = GetDlgItem(hwnd_, IDC_COMBO_OUTPUT);
    SendMessageW(in, CB_RESETCONTENT, 0, 0);
    SendMessageW(out, CB_RESETCONTENT, 0, 0);
    SendMessageW(in, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"自动 Auto (SoundRadar/Voicemeeter)"));
    SendMessageW(out, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"默认设备 Default"));
    for (const DeviceInfo& d : EnumerateEndpoints(eCapture))
        SendMessageW(in, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
    for (const DeviceInfo& d : EnumerateEndpoints(eRender)) {
        std::wstring label = d.name;
        if (IsVirtualAudioName(d.name)) label += L" (虚拟)";
        SendMessageW(out, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const AppConfig& cfg = *hooks_.cfg;
    auto selectByName = [](HWND combo, const std::wstring& name) {
        int n = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        for (int i = 1; i < n; ++i) {
            wchar_t buf[256] = {};
            SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
            std::wstring item = buf;
            size_t virt = item.find(L" (虚拟)");
            if (virt != std::wstring::npos) item.erase(virt);
            if (item == name || NameContainsAll(item, name)) {
                SendMessageW(combo, CB_SETCURSEL, i, 0);
                return;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    };
    if (cfg.captureDevice.empty() || cfg.captureDevice == L"SoundRadar")
        SendMessageW(in, CB_SETCURSEL, 0, 0);
    else
        selectByName(in, cfg.captureDevice);
    if (cfg.outputDevice.empty())
        SendMessageW(out, CB_SETCURSEL, 0, 0);
    else
        selectByName(out, cfg.outputDevice);
}

std::wstring Gui::ComboSelection(int id, bool input) const {
    HWND combo = GetDlgItem(hwnd_, id);
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel <= 0) return input ? L"SoundRadar" : L""; // Auto / Default entries
    wchar_t buf[256] = {};
    SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf));
    std::wstring item = buf;
    size_t virt = item.find(L" (虚拟)");
    if (virt != std::wstring::npos) item.erase(virt);
    return item;
}

void Gui::LoadFromConfig() {
    const AppConfig& cfg = *hooks_.cfg;
    CheckRadioButton(hwnd_, IDC_RADIO_MONO, IDC_RADIO_STEREO,
                     cfg.downmix.mode == DownmixStereo ? IDC_RADIO_STEREO : IDC_RADIO_MONO);
    CheckDlgButton(hwnd_, IDC_CHK_OVERLAY, cfg.overlay.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_CHK_CLASSIFY,
                   cfg.classifyEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_CHK_AUTOSTART,
                   AutostartIsEnabled() ? BST_CHECKED : BST_UNCHECKED);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    auto clamp = [](int v, int mn, int mx) { return v < mn ? mn : (v > mx ? mx : v); };
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_FADE), TBM_SETPOS, TRUE,
                 clamp(cfg.analysis.fadeMs, kFadeMin, kFadeMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_RADIUS), TBM_SETPOS, TRUE,
                 clamp(cfg.overlay.radius, kRadiusMin, kRadiusMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_LOW), TBM_SETPOS, TRUE,
                 clamp(static_cast<int>(cfg.overlay.lowThreshold * 100 + 0.5f), kThrMin,
                       kThrMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_HIGH), TBM_SETPOS, TRUE,
                 clamp(static_cast<int>(cfg.overlay.highThreshold * 100 + 0.5f), kThrMin,
                       kThrMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_POSX), TBM_SETPOS, TRUE,
                 clamp(cfg.overlay.offsetX * 100 / sw, kPosXMin, kPosXMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_POSY), TBM_SETPOS, TRUE,
                 clamp(cfg.overlay.offsetY * 100 / sh, kPosYMin, kPosYMax));
    SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_FX), TBM_SETPOS, TRUE,
                 clamp(cfg.overlay.fxPct, kFxMin, kFxMax));
    for (int i = 0; i < 8; ++i) {
        SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_W0 + i), TBM_SETPOS, TRUE,
                     clamp(static_cast<int>(cfg.downmix.weights[i] * 20 + 0.5f), 0, kWMax));
    }
    UpdateSliderLabels();
}

void Gui::UpdateSliderLabels() {
    auto pos = [&](int id) {
        return static_cast<int>(
            SendMessageW(GetDlgItem(hwnd_, id), TBM_GETPOS, 0, 0));
    };
    wchar_t buf[64];
    auto setText = [&](int id) { SetWindowTextW(GetDlgItem(hwnd_, id + 500), buf); };
    swprintf_s(buf, L"%d ms", pos(IDC_SLIDER_FADE));
    setText(IDC_SLIDER_FADE);
    swprintf_s(buf, L"%d px", pos(IDC_SLIDER_RADIUS));
    setText(IDC_SLIDER_RADIUS);
    swprintf_s(buf, L"%.2f", pos(IDC_SLIDER_LOW) / 100.0);
    setText(IDC_SLIDER_LOW);
    swprintf_s(buf, L"%.2f", pos(IDC_SLIDER_HIGH) / 100.0);
    setText(IDC_SLIDER_HIGH);
    swprintf_s(buf, L"%+d%%", pos(IDC_SLIDER_POSX));
    setText(IDC_SLIDER_POSX);
    swprintf_s(buf, L"%d%%", pos(IDC_SLIDER_POSY));
    setText(IDC_SLIDER_POSY);
    swprintf_s(buf, L"%d%%", pos(IDC_SLIDER_FX));
    setText(IDC_SLIDER_FX);
}

void Gui::Apply() {
    if (applying_ || !hooks_.cfg) return;
    applying_ = true;
    AppConfig& cfg = *hooks_.cfg;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    auto pos = [&](int id) {
        return static_cast<int>(SendMessageW(GetDlgItem(hwnd_, id), TBM_GETPOS, 0, 0));
    };

    std::wstring newInput = ComboSelection(IDC_COMBO_INPUT, true);
    std::wstring newOutput = ComboSelection(IDC_COMBO_OUTPUT, false);
    bool devChanged = (newInput != appliedInput_) || (newOutput != appliedOutput_);
    cfg.captureDevice = newInput;
    cfg.outputDevice = newOutput;

    cfg.downmix.mode = IsDlgButtonChecked(hwnd_, IDC_RADIO_STEREO) == BST_CHECKED
                           ? DownmixStereo
                           : DownmixRightMono;
    cfg.overlay.enabled = IsDlgButtonChecked(hwnd_, IDC_CHK_OVERLAY) == BST_CHECKED;
    cfg.classifyEnabled = IsDlgButtonChecked(hwnd_, IDC_CHK_CLASSIFY) == BST_CHECKED;
    bool wantAutostart = IsDlgButtonChecked(hwnd_, IDC_CHK_AUTOSTART) == BST_CHECKED;
    if (wantAutostart != AutostartIsEnabled()) AutostartSet(wantAutostart);
    cfg.autostart = wantAutostart;

    cfg.analysis.fadeMs = pos(IDC_SLIDER_FADE);
    cfg.overlay.radius = pos(IDC_SLIDER_RADIUS);
    cfg.overlay.lowThreshold = pos(IDC_SLIDER_LOW) / 100.0f;
    cfg.overlay.highThreshold = pos(IDC_SLIDER_HIGH) / 100.0f;
    if (cfg.overlay.highThreshold < cfg.overlay.lowThreshold + 0.05f)
        cfg.overlay.highThreshold = cfg.overlay.lowThreshold + 0.05f;
    cfg.overlay.offsetX = pos(IDC_SLIDER_POSX) * sw / 100;
    cfg.overlay.offsetY = pos(IDC_SLIDER_POSY) * sh / 100;
    cfg.overlay.fxPct = pos(IDC_SLIDER_FX);
    for (int i = 0; i < 8; ++i)
        cfg.downmix.weights[i] = pos(IDC_SLIDER_W0 + i) / 20.0f;

    // hot-apply to the running threads
    {
        std::lock_guard<std::mutex> lk(g_downmix.mu);
        g_downmix.cfg = cfg.downmix;
    }
    {
        std::lock_guard<std::mutex> lk(g_overlay.mu);
        g_overlay.cfg = cfg.overlay;
        ++g_overlay.version;
    }
    {
        std::lock_guard<std::mutex> lk(g_analysis.mu);
        g_analysis.cfg = cfg.analysis;
        ++g_analysis.version;
    }
    g_classifyEnabled.store(cfg.classifyEnabled);

    SaveConfig(hooks_.configPath, cfg);
    Log("gui: applied (devChanged=%d)", devChanged ? 1 : 0);

    appliedInput_ = newInput;
    appliedOutput_ = newOutput;
    applying_ = false;
    if (hooks_.onApply) hooks_.onApply(devChanged);
}

LRESULT Gui::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, bgBrush_);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND child = reinterpret_cast<HWND>(lp);
            SetBkMode(dc, TRANSPARENT);
            for (HWND c : accentStatics_)
                if (c == child) {
                    SetTextColor(dc, kAccent);
                    return reinterpret_cast<LRESULT>(bgBrush_);
                }
            for (HWND c : dimStatics_)
                if (c == child) {
                    SetTextColor(dc, RGB(0x8a, 0x90, 0xa0));
                    return reinterpret_cast<LRESULT>(bgBrush_);
                }
            int id = GetDlgCtrlID(child);
            if (child == stripBg_ || id == IDC_STATUS) {
                SetTextColor(dc, kText);
                return reinterpret_cast<LRESULT>(stripBrush_);
            }
            for (HWND c : sepStatics_)
                if (c == child) {
                    SetTextColor(dc, kSep);
                    SetBkColor(dc, kSep);
                    return reinterpret_cast<LRESULT>(sepBrush_);
                }
            SetTextColor(dc, kText);
            SetBkColor(dc, kBg);
            return reinterpret_cast<LRESULT>(bgBrush_);
        }
        case WM_CTLCOLORBTN: { // radios / checkboxes
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(bgBrush_);
        }
        case WM_CTLCOLORLISTBOX: // combo dropdown list
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kText);
            SetBkColor(dc, kPanel);
            return reinterpret_cast<LRESULT>(stripBrush_);
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
            if (dis->CtlType == ODT_BUTTON) {
                DrawButton(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_STATIC && dis->CtlID == IDC_STATUSDOT) {
                DrawStatusDot(dis);
                return TRUE;
            }
            return FALSE;
        }
        case WM_HSCROLL:
            UpdateSliderLabels();
            return 0;
        case WM_TIMER:
            if (wp == kStatusTimer) {
                if (hooks_.statusText)
                    SetWindowTextW(GetDlgItem(hwnd_, IDC_STATUS),
                                   hooks_.statusText().c_str());
                if (hooks_.statusLevel) {
                    int lv = hooks_.statusLevel();
                    if (lv != statusLevel_) {
                        statusLevel_ = lv;
                        InvalidateRect(GetDlgItem(hwnd_, IDC_STATUSDOT), nullptr, TRUE);
                    }
                }
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_BTN_APPLY:
                    Apply();
                    return 0;
                case IDC_BTN_OK:
                    Apply();
                    Hide();
                    return 0;
                case IDC_BTN_EXIT:
                    if (hooks_.onExit) hooks_.onExit(); // real exit (same path as tray)
                    return 0;
                case IDC_BTN_RESETW:
                    for (int i = 0; i < 8; ++i)
                        SendMessageW(GetDlgItem(hwnd_, IDC_SLIDER_W0 + i), TBM_SETPOS, TRUE,
                                     static_cast<LPARAM>(static_cast<int>(
                                         kDefaultWeights[i] * 20 + 0.5f)));
                    return 0;
                default:
                    return 0;
            }
        case kMsgGuiActivate:
            Show();
            return 0;
        case WM_CLOSE:
            Hide(); // close = minimize to tray, never exit
            return 0;
        case WM_DESTROY:
            if (font_) DeleteObject(font_);
            if (titleFont_) DeleteObject(titleFont_);
            if (bgBrush_) DeleteObject(bgBrush_);
            if (stripBrush_) DeleteObject(stripBrush_);
            if (sepBrush_) DeleteObject(sepBrush_);
            font_ = nullptr;
            titleFont_ = nullptr;
            bgBrush_ = stripBrush_ = sepBrush_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace sr
