// gui2.cpp - WebView2-hosted settings window. The HTML page is embedded in
// the exe (gui_web_html.h, regenerated at build time from gui_web.html).
// Bridge: window.chrome.webview.postMessage, JSON both ways.
#include "gui2.h"

// WIN32_LEAN_AND_MEAN skips the COM headers WebView2.h needs (interface macro,
// EventRegistrationToken): pull them in explicitly.
#include <objbase.h>
#include <eventtoken.h>
#include <WebView2.h>

#include <dwmapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi")

#include <cstdio>
#include <cstring>
#include <functional>

#include "gui_web_html.h" // kGuiHtml (generated)
#include "devicedefault.h"
#include "log.h"
#include "meters.h"
#include "tray.h" // AutostartIsEnabled/AutostartSet
#include "version.h"
#include "wasapi_util.h"

#pragma comment(lib, "shlwapi")

namespace sr {

// --- tiny COM callback plumbing (WRL's Callback<> conflicts with this SDK) ---

template <typename T>
struct RefCounted : public T {
    ULONG refs_ = 1;
    virtual ~RefCounted() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(T)) {
            *ppv = static_cast<T*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
};

struct EnvHandler : RefCounted<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    std::function<HRESULT(HRESULT, ICoreWebView2Environment*)> fn;
    explicit EnvHandler(decltype(fn) f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        return fn(hr, env);
    }
};
struct CtlHandler : RefCounted<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    std::function<HRESULT(HRESULT, ICoreWebView2Controller*)> fn;
    explicit CtlHandler(decltype(fn) f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* c) override {
        return fn(hr, c);
    }
};
struct MsgHandler : RefCounted<ICoreWebView2WebMessageReceivedEventHandler> {
    std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)> fn;
    explicit MsgHandler(decltype(fn) f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* wv,
                                     ICoreWebView2WebMessageReceivedEventArgs* a) override {
        return fn(wv, a);
    }
};
struct CaptureHandler : RefCounted<ICoreWebView2CapturePreviewCompletedHandler> {
    std::function<HRESULT(HRESULT)> fn;
    explicit CaptureHandler(decltype(fn) f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr) override { return fn(hr); }
};
struct ScriptHandler : RefCounted<ICoreWebView2ExecuteScriptCompletedHandler> {
    std::function<HRESULT(HRESULT, LPCWSTR)> fn;
    explicit ScriptHandler(decltype(fn) f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR res) override {
        return fn(hr, res);
    }
};

const wchar_t* kGuiClassName = L"SoundRadarMainWnd";

namespace {

constexpr UINT kStateTimer = 77;
constexpr UINT kFitTimer = 78;
constexpr UINT kDragTimer = 79; // frameless window drag follows the cursor

std::wstring AppDataDir() {
    std::wstring p = DefaultConfigPath();
    return p.substr(0, p.find_last_of(L"\\/"));
}

std::string JEscape(const std::wstring& w) {
    std::string s = ToUtf8(w);
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

// --- minimal JSON reader (flat keys; objects/arrays returned raw) -----------

bool JFindRaw(const std::string& j, const std::string& key, size_t from,
              std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle, from);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size()) return false;
    if (j[p] == '"') {
        size_t e = j.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = j.substr(p + 1, e - p - 1);
        return true;
    }
    if (j[p] == '{' || j[p] == '[') {
        char open = j[p], close = (open == '{') ? '}' : ']';
        int depth = 0;
        for (size_t i = p; i < j.size(); ++i) {
            if (j[i] == open) ++depth;
            else if (j[i] == close && --depth == 0) {
                out = j.substr(p, i - p + 1);
                return true;
            }
        }
        return false;
    }
    size_t e = p;
    while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']') ++e;
    out = j.substr(p, e - p);
    return true;
}

bool JFindRaw(const std::string& j, const std::string& key, std::string& out) {
    return JFindRaw(j, key, 0, out);
}

bool JNum(const std::string& j, const std::string& key, double& v) {
    std::string raw;
    if (!JFindRaw(j, key, raw)) return false;
    char* e = nullptr;
    v = std::strtod(raw.c_str(), &e);
    return e != raw.c_str();
}

bool JBool(const std::string& j, const std::string& key, bool& v) {
    std::string raw;
    if (!JFindRaw(j, key, raw)) return false;
    if (raw == "true") { v = true; return true; }
    if (raw == "false") { v = false; return true; }
    return false;
}

bool JStr(const std::string& j, const std::string& key, std::string& v) {
    return JFindRaw(j, key, v);
}

bool JFloatArray(const std::string& j, const std::string& key, float* dst, size_t count) {
    std::string raw;
    if (!JFindRaw(j, key, raw)) return false;
    if (raw.empty() || raw.front() != '[') return false;
    size_t i = 0;
    const char* p = raw.c_str() + 1;
    while (i < count) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p || *p == ']') break;
        char* e = nullptr;
        float f = std::strtof(p, &e);
        if (e == p) break;
        dst[i++] = f;
        p = e;
    }
    return i == count;
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

    HINSTANCE inst = GetModuleHandleW(nullptr);
    bgBrush_ = CreateSolidBrush(RGB(0x0f, 0x11, 0x17));
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kGuiClassName;
    wc.hbrBackground = bgBrush_;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("gui2: RegisterClassEx failed err=%lu", GetLastError());
        return false;
    }

    // frameless window, rounded corners via DWM; WS_EX_APPWINDOW keeps a
    // taskbar button (popup windows need it explicitly)
    RECT rc = { 0, 0, 560, 900 };
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kGuiClassName, L"SoundRadar 声纹雷达",
                            WS_POPUP, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log("gui2: CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = RGB(0x2a, 0x30, 0x40);
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    MARGINS m = { 0, 0, 0, 1 }; // 1px frame extension -> DWM drop shadow
    DwmExtendFrameIntoClientArea(hwnd_, &m);

    // device lists (index 0 = Auto / Default)
    devIn_.clear();
    devOut_.clear();
    for (const DeviceInfo& d : EnumerateEndpoints(eCapture)) devIn_.push_back(d.name);
    for (const DeviceInfo& d : EnumerateEndpoints(eRender)) {
        devOut_.push_back(d.name + (IsVirtualAudioName(d.name) ? L" (虚拟)" : L""));
    }
    lastInput_ = hooks_.cfg->captureDevice;
    lastOutput_ = hooks_.cfg->outputDevice;

    SetTimer(hwnd_, kStateTimer, 500, nullptr);
    InitWebView();
    if (!hidden) Show();
    Log("gui2: window created (hidden=%d)", hidden ? 1 : 0);
    return true;
}

void Gui::InitWebView() {
    std::wstring udf = AppDataDir() + L"\\webview2";
    CreateDirectoryW(udf.c_str(), nullptr);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        new EnvHandler([this](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                Log("gui2: WebView2 environment failed hr=0x%08lx", (unsigned long)hr);
                return S_OK;
            }
            env->CreateCoreWebView2Controller(
                hwnd_,
                new CtlHandler(
                    [this](HRESULT hr, ICoreWebView2Controller* ctl) -> HRESULT {
                        if (FAILED(hr) || !ctl) {
                            Log("gui2: controller failed hr=0x%08lx", (unsigned long)hr);
                            return S_OK;
                        }
                        controller_ = ctl;
                        ctl->AddRef();
                        ICoreWebView2* wv = nullptr;
                        ctl->get_CoreWebView2(&wv);
                        webview_ = wv; // owned
                        RECT rc;
                        GetClientRect(hwnd_, &rc);
                        ctl->put_Bounds(rc);
                        ctl->put_IsVisible(IsWindowVisible(hwnd_) ? TRUE : FALSE);
                        EventRegistrationToken tok;
                        webview_->add_WebMessageReceived(
                            new MsgHandler(
                                [this](ICoreWebView2*,
                                       ICoreWebView2WebMessageReceivedEventArgs* a)
                                    -> HRESULT {
                                    LPWSTR json = nullptr;
                                    a->get_WebMessageAsJson(&json);
                                    if (json) {
                                        OnBridgeMessage(json);
                                        CoTaskMemFree(json);
                                    }
                                    return S_OK;
                                }),
                            &tok);
                        static std::wstring html = [] {
                            std::wstring s = kGuiHtml;
                            const std::wstring token = L"@APP_VERSION@";
                            size_t p = s.find(token);
                            if (p != std::wstring::npos)
                                s.replace(p, token.size(), SR_APP_VERSION);
                            return s;
                        }();
                        webview_->NavigateToString(html.c_str());
                        webReady_ = true;
                        Log("gui2: webview ready");
                        return S_OK;
                    }));
            return S_OK;
        }));
    if (FAILED(hr)) Log("gui2: CreateCoreWebView2Environment hr=0x%08lx", (unsigned long)hr);
}

void Gui::Show() {
    if (!hwnd_) return;
    if (HiddenTestMode()) {
        Log("gui2: activate requested (hidden test mode)");
        return;
    }
    ShowWindow(hwnd_, SW_RESTORE);
    if (controller_) controller_->put_IsVisible(TRUE);
    HWND fg = GetForegroundWindow();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myT = GetCurrentThreadId();
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
    SetForegroundWindow(hwnd_);
    BringWindowToTop(hwnd_);
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
}

void Gui::Hide() {
    if (!hwnd_) return;
    if (controller_) controller_->put_IsVisible(FALSE);
    ShowWindow(hwnd_, SW_HIDE);
}

bool Gui::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void Gui::PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

bool Gui::EvalJson(const wchar_t* script, std::wstring& resultJson) {
    if (!webview_) return false;
    bool done = false;
    HRESULT shr = E_FAIL;
    HRESULT hr = webview_->ExecuteScript(
        script, new ScriptHandler([&](HRESULT e, LPCWSTR res) -> HRESULT {
            shr = e;
            resultJson = res ? res : L"";
            done = true;
            return S_OK;
        }));
    if (FAILED(hr)) return false;
    uint64_t t0 = GetTickCount64();
    while (!done && GetTickCount64() - t0 < 5000) {
        PumpMessages();
        Sleep(5);
    }
    return done && SUCCEEDED(shr);
}

bool Gui::CapturePng(const std::wstring& path) {
    if (!webview_) return false;
    IStream* stream = nullptr;
    if (FAILED(SHCreateStreamOnFileW(path.c_str(), STGM_CREATE | STGM_WRITE, &stream)))
        return false;
    bool done = false, ok = false;
    HRESULT hr = webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
        new CaptureHandler([&](HRESULT e) -> HRESULT {
            if (FAILED(e)) sr::Log("gui2: CapturePreview err hr=0x%08lx", (unsigned long)e);
            ok = SUCCEEDED(e);
            done = true;
            return S_OK;
        }));
    if (FAILED(hr)) {
        sr::Log("gui2: CapturePreview call failed hr=0x%08lx", (unsigned long)hr);
        stream->Release();
        return false;
    }
    uint64_t t0 = GetTickCount64();
    while (!done && GetTickCount64() - t0 < 5000) {
        PumpMessages();
        Sleep(10);
    }
    if (!done) sr::Log("gui2: CapturePreview timeout");
    stream->Release();
    return done && ok;
}

bool Gui::InjectBridgeMessage(const std::wstring& json) {
    if (!webview_) return false;
    std::wstring script = L"window.chrome.webview.postMessage(" + json + L")";
    webview_->ExecuteScript(script.c_str(), nullptr);
    return true;
}

// --- state push (C++ -> JS) --------------------------------------------------

void Gui::PushState() {
    if (!webview_ || !pageReady_) return;
    const AppConfig& cfg = *hooks_.cfg;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    // current selections: match config value against the lists
    int selIn = 0;
    if (!(cfg.captureDevice.empty() || cfg.captureDevice == L"SoundRadar")) {
        for (size_t i = 0; i < devIn_.size(); ++i)
            if (devIn_[i] == cfg.captureDevice ||
                NameContainsAll(devIn_[i], cfg.captureDevice)) {
                selIn = static_cast<int>(i) + 1;
                break;
            }
    }
    int selOut = 0;
    if (!cfg.outputDevice.empty()) {
        for (size_t i = 0; i < devOut_.size(); ++i)
            if (NameContainsAll(devOut_[i], cfg.outputDevice)) {
                selOut = static_cast<int>(i) + 1;
                break;
            }
    }

    std::string j;
    j.reserve(2048);
    j += "{\"type\":\"state\",\"devicesIn\":[\"自动 Auto (SoundRadar/Voicemeeter)\"";
    for (const auto& n : devIn_) { j += ",\""; j += JEscape(n); j += "\""; }
    j += "],\"devicesOut\":[\"默认设备 Default\"";
    for (const auto& n : devOut_) { j += ",\""; j += JEscape(n); j += "\""; }
    j += "]";
    char num[64];
    auto addNum = [&](const char* k, double v) {
        std::snprintf(num, sizeof(num), ",\"%s\":%g", k, v);
        j += num;
    };
    std::snprintf(num, sizeof(num), ",\"selIn\":%d,\"selOut\":%d", selIn, selOut);
    j += num;
    j += ",\"config\":{";
    j += std::string("\"mode\":\"") + (cfg.downmix.mode == DownmixStereo ? "stereo" : "right-mono") + "\"";
    addNum("fade_ms", cfg.analysis.fadeMs);
    addNum("radar_radius", cfg.overlay.radius);
    addNum("overlay_low", cfg.overlay.lowThreshold);
    addNum("overlay_high", cfg.overlay.highThreshold);
    addNum("pos_x_pct", cfg.overlay.offsetX * 100.0 / sw);
    addNum("pos_y_pct", cfg.overlay.offsetY * 100.0 / sh);
    addNum("overlay_fx", cfg.overlay.fxPct);
    addNum("sensitivity", cfg.overlay.sensitivity);
    j += ",\"overlay_enabled\":"; j += cfg.overlay.enabled ? "true" : "false";
    j += ",\"classify_enabled\":"; j += cfg.classifyEnabled ? "true" : "false";
    j += ",\"autostart\":"; j += AutostartIsEnabled() ? "true" : "false";
    j += ",\"weights\":[";
    for (int i = 0; i < 8; ++i) {
        std::snprintf(num, sizeof(num), i ? ",%.3f" : "%.3f",
                      resetPending_ ? (i == 1 || i == 2 ? 1.0 : (i == 0 || i == 3 ? 0.7 :
                                      (i == 4 || i == 5 ? 0.8 : 0.9)))
                                      : static_cast<double>(cfg.downmix.weights[i]));
        j += num;
    }
    resetPending_ = false;
    j += "]}";
    // status
    std::wstring st = hooks_.statusText ? hooks_.statusText() : L"";
    int lvl = hooks_.statusLevel ? hooks_.statusLevel() : 1;
    j += ",\"status\":{\"running\":";
    j += (lvl == 0) ? "true" : "false";
    j += ",\"error\":";
    j += (lvl == 2) ? "true" : "false";
    j += ",\"text\":\"";
    std::string st8;
    for (wchar_t c : st) {
        if (c == L'\r') continue;
        if (c == L'\n') st8 += " | ";
        else { wchar_t tmp[2] = { c, 0 }; st8 += JEscape(tmp); }
    }
    j += st8;
    j += "\"}";
    // live levels
    std::array<float, 8> levels{};
    if (hooks_.levels) levels = hooks_.levels();
    j += ",\"levels\":[";
    for (int i = 0; i < 8; ++i) {
        std::snprintf(num, sizeof(num), i ? ",%.3f" : "%.3f",
                      static_cast<double>(levels[i]));
        j += num;
    }
    j += "]}";

    std::wstring wj;
    int n = MultiByteToWideChar(CP_UTF8, 0, j.c_str(), -1, nullptr, 0);
    wj.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, j.c_str(), -1, wj.data(), n);
    webview_->PostWebMessageAsJson(wj.c_str());
}

// --- bridge (JS -> C++) -------------------------------------------------------

void Gui::OnBridgeMessage(const wchar_t* jsonW) {
    std::string j = ToUtf8(jsonW);
    std::string cmd;
    if (!JStr(j, "cmd", cmd)) return;
    if (cmd == "debugClick") {
        std::string tgt;
        JStr(j, "id", tgt);
        sr::Log("gui2: JS click received, target=[%s]", tgt.c_str());
        return;
    }
    if (cmd == "ready") {
        pageReady_ = true;
        PushState();
        SetTimer(hwnd_, kFitTimer, 400, nullptr); // auto-fit once laid out
        return;
    }
    if (cmd == "drag") {
        // frameless title-bar drag: follow the cursor on a timer. (A modal
        // HTCAPTION move loop would swallow the in-flight click's mouse-up and
        // wedge WebView2 input. End via WM_LBUTTONUP + async-key fallback.)
        SetCapture(hwnd_);
        // leave the page unclickable.)
        GetCursorPos(&dragCursor_);
        SetTimer(hwnd_, kDragTimer, 30, nullptr);
        return;
    }
    if (cmd == "setDefault") {
        double selIn = 0;
        JNum(j, "selIn", selIn);
        std::wstring capName = (selIn >= 1 && selIn <= (int)devIn_.size())
                                   ? devIn_[static_cast<int>(selIn) - 1]
                                   : L"SoundRadar";
        std::wstring needle = RenderCounterpartNeedle(capName);
        bool okSet = !needle.empty() && SetDefaultRenderDevice(needle);
        Log("gui2: set-default '%s' -> '%s': %s", ToUtf8(capName).c_str(),
            ToUtf8(needle).c_str(), okSet ? "ok" : "FAILED");
        return;
    }
    if (cmd == "exit") {
        if (hooks_.onExit) hooks_.onExit();
        return;
    }
    if (cmd == "minimize") {
        Hide();
        return;
    }
    if (cmd == "resetWeights") {
        resetPending_ = true; // next PushState carries defaults
        PushState();
        return;
    }
    if (cmd == "apply") {
        double selIn = 0, selOut = 0;
        JNum(j, "selIn", selIn);
        JNum(j, "selOut", selOut);
        std::string cfgJson;
        if (JFindRaw(j, "config", cfgJson))
            ApplyFromJson(cfgJson, static_cast<int>(selIn), static_cast<int>(selOut));
        return;
    }
}

void Gui::ApplyFromJson(const std::string& cj, int selIn, int selOut) {
    if (!hooks_.cfg) return;
    AppConfig& cfg = *hooks_.cfg;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    std::string s;
    if (JStr(cj, "mode", s))
        cfg.downmix.mode = (s == "stereo") ? DownmixStereo : DownmixRightMono;
    JBool(cj, "overlay_enabled", cfg.overlay.enabled);
    JBool(cj, "classify_enabled", cfg.classifyEnabled);
    double d;
    if (JNum(cj, "fade_ms", d)) cfg.analysis.fadeMs = static_cast<int>(d);
    if (JNum(cj, "radar_radius", d)) cfg.overlay.radius = static_cast<int>(d);
    if (JNum(cj, "overlay_low", d)) cfg.overlay.lowThreshold = static_cast<float>(d);
    if (JNum(cj, "overlay_high", d)) cfg.overlay.highThreshold = static_cast<float>(d);
    if (cfg.overlay.highThreshold < cfg.overlay.lowThreshold + 0.05f)
        cfg.overlay.highThreshold = cfg.overlay.lowThreshold + 0.05f;
    if (JNum(cj, "pos_x_pct", d))
        cfg.overlay.offsetX = static_cast<int>(d) * sw / 100;
    if (JNum(cj, "pos_y_pct", d))
        cfg.overlay.offsetY = static_cast<int>(d) * sh / 100;
    if (JNum(cj, "overlay_fx", d)) cfg.overlay.fxPct = static_cast<int>(d);
    if (JNum(cj, "sensitivity", d)) cfg.overlay.sensitivity = static_cast<float>(d);
    JFloatArray(cj, "weights", cfg.downmix.weights, 8);

    bool wantAuto = AutostartIsEnabled();
    if (JBool(cj, "autostart", wantAuto) && wantAuto != AutostartIsEnabled())
        AutostartSet(wantAuto);
    cfg.autostart = wantAuto;

    // device selection by dropdown index
    std::wstring newInput = (selIn <= 0) ? L"SoundRadar"
                          : (selIn <= (int)devIn_.size() ? devIn_[selIn - 1] : L"SoundRadar");
    std::wstring newOut;
    if (selOut > 0 && selOut <= (int)devOut_.size()) {
        newOut = devOut_[selOut - 1];
        size_t v = newOut.find(L" (虚拟)");
        if (v != std::wstring::npos) newOut.erase(v);
    }
    bool devChanged = (newInput != lastInput_) || (newOut != lastOutput_);
    cfg.captureDevice = newInput;
    cfg.outputDevice = newOut;
    lastInput_ = newInput;
    lastOutput_ = newOut;

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
    Log("gui2: applied (devChanged=%d)", devChanged ? 1 : 0);
    if (hooks_.onApply) hooks_.onApply(devChanged);
    PushState();
}

// Sizes the frameless window to the measured page content (no scrollbar ever).
void Gui::MeasureAndFit() {
    std::wstring r;
    if (!EvalJson(L"String(document.documentElement.scrollHeight)", r))
        return;
    int contentH = 0;
    for (wchar_t c : r)
        if (c >= L'0' && c <= L'9') contentH = contentH * 10 + (c - L'0');
    if (contentH < 400) return;
    // scrollHeight is in CSS px; the window is sized in physical px
    double scale = GetDpiForWindow(hwnd_) / 96.0;
    contentH = static_cast<int>(contentH * scale + 0.5);
    int maxH = GetSystemMetrics(SM_CYSCREEN) - 60;
    if (contentH > maxH) contentH = maxH;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int curH = cr.bottom - cr.top;
    if (curH == contentH) return;
    SetWindowPos(hwnd_, nullptr, 0, 0, cr.right - cr.left, contentH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT after;
    GetClientRect(hwnd_, &after);
    Log("gui2: auto-fit height %d -> %d (actual client %d)", curH, contentH,
        after.bottom - after.top);
}

// --- window proc ---------------------------------------------------------------

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

LRESULT Gui::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            sr::Log("gui2: WM_LBUTTONDOWN at %d,%d", (int)(short)LOWORD(lp),
                    (int)(short)HIWORD(lp));
            break;
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, bgBrush_);
            return 1;
        }
        case WM_SIZE:
            if (controller_) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                controller_->put_Bounds(rc);
            }
            return 0;
        case WM_TIMER:
            if (wp == kStateTimer) PushState();
            else if (wp == kFitTimer) {
                KillTimer(hwnd_, kFitTimer);
                MeasureAndFit();
            } else if (wp == kDragTimer) {
                // End on async state OR via WM_LBUTTONUP below; remote
                // sessions (ToDesk) may not report async key state.
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                    KillTimer(hwnd_, kDragTimer);
                    ReleaseCapture();
                } else {
                    POINT cur;
                    GetCursorPos(&cur);
                    RECT wr;
                    GetWindowRect(hwnd_, &wr);
                    SetWindowPos(hwnd_, nullptr, wr.left + cur.x - dragCursor_.x,
                                 wr.top + cur.y - dragCursor_.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    dragCursor_ = cur;
                }
            }
            return 0;
        case kMsgGuiActivate:
            Show();
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            KillTimer(hwnd_, kDragTimer);
            ReleaseCapture();
            return 0;
        case WM_CLOSE:
            Hide(); // close = minimize to tray, never exit
            return 0;
        case WM_DESTROY:
            if (controller_) {
                controller_->Close();
                controller_->Release();
                controller_ = nullptr;
            }
            if (webview_) {
                webview_->Release();
                webview_ = nullptr;
            }
            if (bgBrush_) DeleteObject(bgBrush_);
            bgBrush_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace sr
