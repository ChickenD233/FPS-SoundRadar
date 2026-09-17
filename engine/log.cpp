#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "config.h" // DefaultConfigPath
#include "wasapi_util.h" // ToUtf8

namespace sr {

namespace {
std::mutex g_mu;
std::wstring g_path;
bool g_ready = false;
}

void LogInit() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::wstring cfg = DefaultConfigPath(); // ...\SoundRadar\config.json
    size_t slash = cfg.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : cfg.substr(0, slash);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_path = dir + L"\\log.txt";
    g_ready = true;
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ready) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path.c_str(), L"ab") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\n", st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    std::fclose(f);
}

void LogW(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, sizeof(msg) / sizeof(msg[0]), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("%s", ToUtf8(msg).c_str());
}

} // namespace sr
