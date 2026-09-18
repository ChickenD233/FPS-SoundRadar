// gui.h - main window (plain Win32, DPI-aware, Chinese-primary bilingual).
// Created on the main thread; shown on double-click, hidden to tray on close.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "config.h"

namespace sr {

// Control IDs (shared with --guitest).
enum GuiControlId {
    IDC_COMBO_INPUT = 2000,
    IDC_COMBO_OUTPUT,
    IDC_RADIO_MONO,
    IDC_RADIO_STEREO,
    IDC_CHK_OVERLAY,
    IDC_SLIDER_FADE,
    IDC_SLIDER_RADIUS,
    IDC_SLIDER_LOW,
    IDC_SLIDER_HIGH,
    IDC_SLIDER_POSX,
    IDC_SLIDER_POSY,
    IDC_SLIDER_FX,
    IDC_SLIDER_W0, // .. +7 for FL FR C LFE BL BR SL SR
    IDC_SLIDER_W7 = IDC_SLIDER_W0 + 7,
    IDC_BTN_RESETW,
    IDC_CHK_CLASSIFY,
    IDC_CHK_AUTOSTART,
    IDC_STATUS,
    IDC_STATUSDOT,
    IDC_BTN_APPLY,
    IDC_BTN_OK,
    IDC_BTN_EXIT,
};

extern const wchar_t* kGuiClassName;       // "SoundRadarMainWnd" (singleton lookup)
constexpr UINT kMsgGuiActivate = WM_APP + 101; // second instance -> show window

class Gui {
public:
    struct Hooks {
        AppConfig* cfg = nullptr;      // live config (main thread only)
        std::wstring configPath;       // where Apply saves
        // devChanged = input/output device selection changed (pipeline restart)
        std::function<void(bool devChanged)> onApply;
        std::function<std::wstring()> statusText;  // status bar provider (500 ms)
        std::function<int()> statusLevel;          // 0=running 1=waiting 2=error
        std::function<void()> onExit;              // 退出程序 button: real app exit
    };

    bool Create(const Hooks& hooks, bool hidden); // false = creation failed
    void Show();  // show, restore, bring to front
    void Hide();  // minimize to tray (window stays alive)
    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const;

    // Reads all controls into cfg, saves config, hot-applies globals.
    // Called by Apply/OK buttons and by the tests.
    void Apply();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BtnSubProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void BuildControls();
    void RefreshDevices();
    void LoadFromConfig();
    void UpdateSliderLabels();
    std::wstring ComboSelection(int id, bool input) const;

    // construction/theme helpers
    HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
            int h, int id, DWORD ex = 0);
    void Caption(const wchar_t* text, int x, int y, int w);
    HWND Slider(int x, int y, int w, int id, int mn, int mx);
    HWND FlatButton(const wchar_t* text, int x, int y, int w, int id);
    void DrawButton(LPDRAWITEMSTRUCT dis);
    void DrawStatusDot(LPDRAWITEMSTRUCT dis);

    Hooks hooks_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    HBRUSH stripBrush_ = nullptr;
    HBRUSH sepBrush_ = nullptr;
    HWND stripBg_ = nullptr;
    std::vector<HWND> accentStatics_; // cyan captions + title
    std::vector<HWND> dimStatics_;    // dimmer secondary text
    std::vector<HWND> sepStatics_;    // separator lines
    std::vector<HWND> ownerBtns_;     // owner-drawn push buttons
    std::map<HWND, bool> btnHover_;
    int statusLevel_ = 1; // amber until first tick
    // applied device selections (to detect changes on Apply)
    std::wstring appliedInput_, appliedOutput_;
    bool applying_ = false;
};

} // namespace sr
