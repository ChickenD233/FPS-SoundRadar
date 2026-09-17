// tray.h - system tray icon, bilingual menu, autostart registry toggle.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "config.h"
#include "meters.h"

namespace sr {

// HKCU\...\CurrentVersion\Run "SoundRadar" = "<exe>" --tray
bool AutostartIsEnabled();
bool AutostartSet(bool enable);

class Tray {
public:
    struct Handlers {
        std::function<void(int mode)> onMode;   // DownmixMode as int
        std::function<void(bool on)> onOverlay;
        std::function<void(bool on)> onClassify; // experimental sound classification
        std::function<void(bool on)> onAutostart;
        std::function<void()> onExit;
        std::function<void()> onTick;           // 500 ms, used for console meter
    };

    bool Init(const AppConfig& cfg, Handlers handlers);
    // Blocks. Returns when quitEvent is signaled or Exit chosen in the menu.
    void Run(HANDLE quitEvent);
    void Shutdown();

    // Tray balloon notification (non-modal). Call after Init.
    void Notify(const std::wstring& title, const std::wstring& msg);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void ShowMenu();
    void RefreshChecks();

    HWND hwnd_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU modeMenu_ = nullptr;
    HICON icon_ = nullptr;
    Handlers handlers_;
    int mode_ = 0;
    bool overlayOn_ = true;
    bool classifyOn_ = true;
    bool autostartOn_ = false;
    static constexpr UINT kTrayMsg = WM_USER + 100;
    static constexpr UINT kTimerId = 1;
};

} // namespace sr
