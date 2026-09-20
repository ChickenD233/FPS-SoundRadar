// update.cpp - WinINet download + PowerShell swap for the GitHub Releases
// updater. The release zip contains SoundRadar/SoundRadar.exe; a generated
// .ps1 waits for this process to exit, overwrites the exe, relaunches it.
#include "update.h"

#include <windows.h>
#include <shellapi.h>
#include <wininet.h>

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

#include "log.h"
#include "wasapi_util.h" // ToUtf8
#include "version.h"

#pragma comment(lib, "wininet")

namespace sr {
namespace {

constexpr DWORD kTimeoutMs = 15000;
const wchar_t* kReleaseApi =
    L"https://api.github.com/repos/ChickenD233/FPS-SoundRadar/releases/latest";

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

HINTERNET OpenSession() {
    HINTERNET ses = InternetOpenW(L"SoundRadar/" SR_APP_VERSION,
                                  INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!ses) return nullptr;
    DWORD t = kTimeoutMs;
    InternetSetOptionW(ses, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionW(ses, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
    InternetSetOptionW(ses, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    return ses;
}

HINTERNET OpenUrl(HINTERNET ses, const wchar_t* url) {
    return InternetOpenUrlW(ses, url, nullptr, 0,
                            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                                INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI,
                            0);
}

bool CheckHttpStatus(HINTERNET h, std::string& err) {
    DWORD code = 0, sz = sizeof(code);
    HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz,
                   nullptr);
    if (code == 200) return true;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HTTP %lu", (unsigned long)code);
    err = (code == 403) ? "GitHub API 限流 (HTTP 403)" : buf;
    return false;
}

bool HttpGet(const wchar_t* url, std::string& out, std::string& err) {
    HINTERNET ses = OpenSession();
    if (!ses) { err = "InternetOpen 失败"; return false; }
    HINTERNET h = OpenUrl(ses, url);
    if (!h) {
        err = "网络连接失败";
        InternetCloseHandle(ses);
        return false;
    }
    bool ok = CheckHttpStatus(h, err);
    if (ok) {
        char buf[16384];
        DWORD n = 0;
        for (;;) {
            if (!InternetReadFile(h, buf, sizeof(buf), &n)) {
                err = "读取响应失败";
                ok = false;
                break;
            }
            if (n == 0) break;
            out.append(buf, n);
        }
    }
    InternetCloseHandle(h);
    InternetCloseHandle(ses);
    return ok;
}

bool DownloadFile(const std::wstring& url, const std::wstring& dest,
                  const std::function<void(uint64_t)>& onBytes, std::string& err) {
    HINTERNET ses = OpenSession();
    if (!ses) { err = "InternetOpen 失败"; return false; }
    HINTERNET h = OpenUrl(ses, url.c_str());
    if (!h) {
        err = "网络连接失败";
        InternetCloseHandle(ses);
        return false;
    }
    bool ok = CheckHttpStatus(h, err);
    HANDLE f = INVALID_HANDLE_VALUE;
    if (ok) {
        f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            err = "无法写入临时文件";
            ok = false;
        }
    }
    uint64_t total = 0;
    if (ok) {
        char buf[65536];
        DWORD n = 0;
        for (;;) {
            if (!InternetReadFile(h, buf, sizeof(buf), &n)) {
                err = "下载中断";
                ok = false;
                break;
            }
            if (n == 0) break;
            DWORD written = 0;
            if (!WriteFile(f, buf, n, &written, nullptr) || written != n) {
                err = "写入临时文件失败";
                ok = false;
                break;
            }
            total += n;
            if (onBytes) onBytes(total);
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    InternetCloseHandle(h);
    InternetCloseHandle(ses);
    if (!ok) DeleteFileW(dest.c_str());
    if (ok && total == 0) { err = "下载内容为空"; ok = false; }
    return ok;
}

// --- hand-rolled JSON field lookup (JFindRaw style) -------------------------

bool FindJsonString(const std::string& j, const std::string& key, size_t from,
                    std::string& out, size_t& endPos) {
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle, from);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return false;
    out = j.substr(p + 1, e - p - 1);
    endPos = e + 1;
    return true;
}

bool FetchLatestRelease(std::string& tag, std::string& zipUrl, std::string& err) {
    std::string body;
    if (!HttpGet(kReleaseApi, body, err)) return false;
    size_t end = 0;
    if (!FindJsonString(body, "tag_name", 0, tag, end)) {
        err = "响应中缺少 tag_name";
        return false;
    }
    size_t from = 0;
    std::string u;
    while (FindJsonString(body, "browser_download_url", from, u, end)) {
        from = end;
        if (u.size() >= 4 && u.compare(u.size() - 4, 4, ".zip") == 0) {
            zipUrl = u;
            return true;
        }
    }
    err = "未找到 zip 下载资产";
    return false;
}

// --- semver-ish compare ------------------------------------------------------

bool ParseVersion(const std::string& s, int v[3]) {
    size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
    for (int k = 0; k < 3; ++k) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
        int n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            n = n * 10 + (s[i++] - '0');
        v[k] = n;
        if (k < 2) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    return true;
}

bool IsNewer(const std::string& latest, const std::string& current) {
    int a[3], b[3];
    if (!ParseVersion(latest, a) || !ParseVersion(current, b)) return false;
    for (int k = 0; k < 3; ++k)
        if (a[k] != b[k]) return a[k] > b[k];
    return false;
}

std::string StripV(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) return tag.substr(1);
    return tag;
}

// --- install -----------------------------------------------------------------

std::wstring TempDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    return (n > 0 && n < MAX_PATH) ? buf : L".\\";
}

// single-quote a string for embedding in a PowerShell command line
std::wstring PsQuote(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    out += L'\'';
    return out;
}

bool RunHidden(const std::wstring& cmdLine, DWORD waitMs, DWORD& exitCode) {
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, waitMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    exitCode = code;
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    // BOM: PowerShell 5.1 treats BOM-less .ps1 as ANSI, breaking non-ASCII paths
    const char bom[] = "\xEF\xBB\xBF";
    DWORD written = 0;
    bool ok = WriteFile(f, bom, 3, &written, nullptr) &&
              WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written,
                        nullptr);
    CloseHandle(f);
    return ok;
}

bool InstallDownloaded(const std::wstring& zipPath, std::string& err) {
    std::wstring dir = TempDir() + L"SoundRadar-update";
    DWORD code = 1;
    std::wstring clean = L"powershell.exe -NoProfile -Command \"if (Test-Path -LiteralPath " +
                         PsQuote(dir) + L") { Remove-Item -Recurse -Force -LiteralPath " +
                         PsQuote(dir) + L" }\"";
    RunHidden(clean, 30000, code); // best effort
    std::wstring expand = L"powershell.exe -NoProfile -Command \"Expand-Archive -Force -LiteralPath " +
                          PsQuote(zipPath) + L" -DestinationPath " + PsQuote(dir) + L"\"";
    if (!RunHidden(expand, 120000, code) || code != 0) {
        err = "解压失败";
        return false;
    }
    std::wstring exe = dir + L"\\SoundRadar\\SoundRadar.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exe = dir + L"\\SoundRadar.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "解压后未找到 SoundRadar.exe";
            return false;
        }
    }
    wchar_t selfBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfBuf, MAX_PATH);
    std::wstring self = selfBuf;
    std::wstring ps1 = TempDir() + L"SoundRadar-update.ps1";
    char pidBuf[32];
    std::snprintf(pidBuf, sizeof(pidBuf), "%lu", (unsigned long)GetCurrentProcessId());
    std::string script;
    script += "$target = " + ToUtf8(PsQuote(self)) + "\r\n";
    script += "$src = " + ToUtf8(PsQuote(exe)) + "\r\n";
    script += "while (Get-Process -Id ";
    script += pidBuf;
    script += " -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }\r\n";
    script += "Start-Sleep -Milliseconds 500\r\n";
    script += "Copy-Item -Force -LiteralPath $src -Destination $target\r\n";
    script += "Start-Process -FilePath $target\r\n";
    script += "Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -Force\r\n";
    if (!WriteFileUtf8(ps1, script)) {
        err = "无法写入更新脚本";
        return false;
    }
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                        ps1 + L"\"";
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(),
                                nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        err = "无法启动更新脚本";
        return false;
    }
    return true;
}

} // namespace

void UpdateCheckAsync(std::function<void(const UpdateStatus&)> onDone) {
    std::thread([cb = std::move(onDone)]() mutable {
        UpdateStatus st;
        st.state = 1;
        cb(st);
        std::string tag, url, err;
        if (!FetchLatestRelease(tag, url, err)) {
            st.state = 6;
            st.message = err;
            Log("update: check failed: %s", err.c_str());
            cb(st);
            return;
        }
        st.latestVersion = StripV(tag);
        st.state = IsNewer(tag, SR_APP_VERSION_A) ? 3 : 2;
        Log("update: check ok, latest=%s state=%d", st.latestVersion.c_str(), st.state);
        cb(st);
    }).detach();
}

void UpdateDownloadAndInstallAsync(std::function<void(const UpdateStatus&)> onProgress) {
    std::thread([cb = std::move(onProgress)]() mutable {
        UpdateStatus st;
        st.state = 4;
        st.message = "正在下载…";
        std::string tag, url, err;
        if (!FetchLatestRelease(tag, url, err)) {
            st.state = 6;
            st.message = err;
            cb(st);
            return;
        }
        if (!IsNewer(tag, SR_APP_VERSION_A)) {
            st.state = 2;
            st.latestVersion = StripV(tag);
            cb(st);
            return;
        }
        st.latestVersion = StripV(tag);
        cb(st); // state 4
        std::wstring zip = TempDir() + L"SoundRadar-update.zip";
        DeleteFileW(zip.c_str());
        uint64_t lastReport = 0;
        bool ok = DownloadFile(Utf8ToWide(url), zip, [&](uint64_t n) {
            if (n - lastReport < 512 * 1024) return;
            lastReport = n;
            UpdateStatus p;
            p.state = 4;
            p.latestVersion = st.latestVersion;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "正在下载… %.1f MB", n / 1048576.0);
            p.message = buf;
            cb(p);
        }, err);
        if (!ok) {
            st.state = 6;
            st.message = "下载失败: " + err;
            Log("update: download failed: %s", err.c_str());
            cb(st);
            return;
        }
        if (!InstallDownloaded(zip, err)) {
            st.state = 6;
            st.message = err;
            Log("update: install failed: %s", err.c_str());
            cb(st);
            return;
        }
        st.state = 5;
        st.message = "即将重启完成更新…";
        Log("update: ready, swap script launched");
        cb(st);
    }).detach();
}

} // namespace sr
