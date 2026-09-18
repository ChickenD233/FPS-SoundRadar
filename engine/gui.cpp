// gui.cpp - main settings window. Plain Win32 + common controls, DPI-aware.
#include "gui.h"

#include <commctrl.h>

#include <cstdio>

#include "log.h"
#include "meters.h" // g_downmix / g_overlay / g_analysis / g_classifyEnabled
#include "tray.h"   // AutostartIsEnabled/AutostartSet
#include "wasapi_util.h"

#pragma comment(lib, "comctl32")

namespace sr {

const wchar_t* kGuiClassName = L"SoundRadarMainWnd";

namespace {

constexpr UINT kStatusTimer = 42;

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

struct ChildEnumCtx {
    std::vector<HWND>* list;
};
BOOL CALLBACK CollectChildren(HWND child, LPARAM lp) {
    reinterpret_cast<ChildEnumCtx*>(lp)->list->push_back(child);
    return TRUE;
}

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

bool Gui::Create(const Hooks& hooks, bool hidden) {
    hooks_ = hooks;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc); // trackbars

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kGuiClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("gui: RegisterClassEx failed err=%lu", GetLastError());
        return false;
    }

    int w = 520, h = 700;
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
    BuildControls();
    RefreshDevices();
    LoadFromConfig();
    appliedInput_ = hooks_.cfg->captureDevice;
    appliedOutput_ = hooks_.cfg->outputDevice;

    // font for every control
    std::vector<HWND> kids;
    ChildEnumCtx ctx{ &kids };
    EnumChildWindows(hwnd_, CollectChildren, reinterpret_cast<LPARAM>(&ctx));
    for (HWND k : kids)
        SendMessageW(k, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

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
    // cross-process foreground needs the input-attach trick
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

// --- control construction ----------------------------------------------------

namespace {

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
        int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND GroupBox(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return Mk(parent, L"BUTTON", text, BS_GROUPBOX, x, y, w, h, -1);
}

HWND Slider(HWND parent, int x, int y, int w, int id, int mn, int mx) {
    HWND s = Mk(parent, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, x, y, w, 22, id);
    SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(mn, mx));
    SendMessageW(s, TBM_SETPAGESIZE, 0, (mx - mn) / 10 + 1);
    return s;
}

} // namespace

void Gui::BuildControls() {
    // 设备 Devices
    GroupBox(hwnd_, L"设备 Devices", 10, 8, 500, 92);
    Mk(hwnd_, L"STATIC", L"输入/捕获 Input", SS_LEFT, 22, 32, 96, 18, -1);
    Mk(hwnd_, L"STATIC", L"输出 Output", SS_LEFT, 22, 64, 96, 18, -1);
    Mk(hwnd_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 122, 28, 376, 300,
       IDC_COMBO_INPUT);
    Mk(hwnd_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 122, 60, 376, 300,
       IDC_COMBO_OUTPUT);

    // 模式 Mode
    GroupBox(hwnd_, L"模式 Mode", 10, 108, 500, 44);
    Mk(hwnd_, L"BUTTON", L"右耳单声道 Right-Mono", BS_AUTORADIOBUTTON | WS_GROUP, 22, 128,
       180, 20, IDC_RADIO_MONO);
    Mk(hwnd_, L"BUTTON", L"立体声 Stereo", BS_AUTORADIOBUTTON, 210, 128, 150, 20,
       IDC_RADIO_STEREO);

    // 声纹样式 Overlay style
    GroupBox(hwnd_, L"声纹样式 Overlay style", 10, 160, 500, 248);
    Mk(hwnd_, L"BUTTON", L"显示声纹 Show overlay", BS_AUTOCHECKBOX, 22, 182, 180, 20,
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
    int y = 210;
    for (const Row& r : rows) {
        Mk(hwnd_, L"STATIC", r.label, SS_LEFT, 22, y + 2, 96, 18, -1);
        Slider(hwnd_, 122, y, 290, r.id, r.mn, r.mx);
        Mk(hwnd_, L"STATIC", L"", SS_LEFT, 418, y + 2, 80, 18, r.id + 500); // value label
        y += 28;
    }

    // 8 声道权重 Weights
    GroupBox(hwnd_, L"8 声道权重 Weights", 10, 416, 500, 124);
    Mk(hwnd_, L"BUTTON", L"重置 Reset", BS_PUSHBUTTON, 425, 418, 75, 22, IDC_BTN_RESETW);
    for (int i = 0; i < 8; ++i) {
        int col = i % 4, row = i / 4;
        int x = 22 + col * 122;
        int ly = 444 + row * 56;
        Mk(hwnd_, L"STATIC", kWeightNames[i], SS_LEFT, x, ly, 40, 14, -1);
        HWND s = Slider(hwnd_, x, ly + 14, 108, IDC_SLIDER_W0 + i, 0, kWMax);
        SendMessageW(s, TBM_SETTICFREQ, 5, 0);
    }

    // 其他 Other
    GroupBox(hwnd_, L"其他 Other", 10, 548, 500, 44);
    Mk(hwnd_, L"BUTTON", L"声音分类 (实验性)", BS_AUTOCHECKBOX, 22, 568, 200, 20,
       IDC_CHK_CLASSIFY);
    Mk(hwnd_, L"BUTTON", L"开机自启 Autostart", BS_AUTOCHECKBOX, 240, 568, 160, 20,
       IDC_CHK_AUTOSTART);

    // buttons + status
    Mk(hwnd_, L"BUTTON", L"退出程序 Exit", BS_PUSHBUTTON, 180, 604, 100, 26, IDC_BTN_EXIT);
    Mk(hwnd_, L"BUTTON", L"应用 Apply", BS_PUSHBUTTON, 290, 604, 100, 26, IDC_BTN_APPLY);
    Mk(hwnd_, L"BUTTON", L"确定 OK", BS_DEFPUSHBUTTON, 400, 604, 100, 26, IDC_BTN_OK);
    Mk(hwnd_, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 10, 640, 500, 52, IDC_STATUS,
       WS_EX_CLIENTEDGE);
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
    // re-apply selection from config
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
        case WM_HSCROLL:
            UpdateSliderLabels();
            return 0;
        case WM_TIMER:
            if (wp == kStatusTimer && hooks_.statusText)
                SetWindowTextW(GetDlgItem(hwnd_, IDC_STATUS), hooks_.statusText().c_str());
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
            font_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace sr
