// gui2.h - main settings window hosting a WebView2 (embedded HTML UI).
// Same window role as the old Win32 GUI: taskbar button, close = tray,
// tray "打开主界面" reopens it. Chinese-primary, dark modern theme.
#pragma once

#include <windows.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "config.h"

// WebView2 interfaces live in the global namespace
struct ICoreWebView2;
struct ICoreWebView2Controller;

namespace sr {

extern const wchar_t* kGuiClassName;      // "SoundRadarMainWnd" (singleton lookup)
constexpr UINT kMsgGuiActivate = WM_APP + 101; // second instance -> show window

class Gui {
public:
    struct Hooks {
        AppConfig* cfg = nullptr;      // live config (main thread only)
        std::wstring configPath;       // where Apply saves
        // devChanged = input/output device selection changed (pipeline restart)
        std::function<void(bool devChanged)> onApply;
        std::function<void()> onExit;      // 退出程序
        std::function<std::wstring()> statusText;  // status line (500 ms)
        std::function<int()> statusLevel;          // 0=running 1=waiting 2=error
        std::function<std::array<float, 8>()> levels; // live channel levels
    };

    bool Create(const Hooks& hooks, bool hidden);
    void Show();
    void Hide();
    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const;

    bool WebViewReady() const { return webReady_; }
    bool PageReady() const { return pageReady_; } // JS bridge handshake done

    // Saves a PNG screenshot of the page via ICoreWebView2::CapturePreview.
    // Returns false if the WebView has never rendered (hidden window).
    bool CapturePng(const std::wstring& path);

    // Test hook: inject a JS bridge message as if the page sent it.
    // Returns false if the bridge is not ready yet.
    bool InjectBridgeMessage(const std::wstring& json);

    // Test hook: run JS and return its JSON result (synchronous; pumps messages).
    bool EvalJson(const wchar_t* script, std::wstring& resultJson);

    // Message pump helper (tests drive the loop manually).
    void PumpMessages();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void InitWebView();
    void PushState();                 // C++ -> JS full state
    void OnBridgeMessage(const wchar_t* json); // JS -> C++
    void ApplyFromJson(const std::string& json, int selIn, int selOut);
    void MeasureAndFit(); // size the frameless window to the page content

    Hooks hooks_;
    HWND hwnd_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    std::wstring lastInput_, lastOutput_; // applied device selections
    bool webReady_ = false;
    bool pageReady_ = false;
    bool resetPending_ = false; // push default weights on next state
    std::vector<std::wstring> devIn_, devOut_; // dropdown name lists
    POINT dragCursor_ = {};                    // frameless drag tracking

    struct ICoreWebView2* webview_ = nullptr;
    struct ICoreWebView2Controller* controller_ = nullptr;
};

} // namespace sr
