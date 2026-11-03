#pragma once

#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

inline std::string u8(const std::wstring & w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t) n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wu(const std::string & s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

inline std::wstring fmt_clock(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t h = ms / 3600000;
    int64_t m = (ms / 60000) % 60;
    int64_t s = (ms / 1000) % 60;
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld", h, m, s);
    return std::wstring(buf);
}

inline std::wstring fmt_srt_time(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t h = ms / 3600000;
    int64_t m = (ms / 60000) % 60;
    int64_t s = (ms / 1000) % 60;
    int64_t t = ms % 1000;
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld,%03lld", h, m, s, t);
    return std::wstring(buf);
}

inline std::wstring replace_extension(const std::wstring & path, const wchar_t * ext) {
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) {
        return path + ext;
    }
    return path.substr(0, dot) + ext;
}

inline std::wstring file_name_of(const std::wstring & path) {
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

inline std::wstring local_appdata() {
    wchar_t base[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        return std::wstring(base);
    }
    const wchar_t * e = _wgetenv(L"LOCALAPPDATA");
    return e ? std::wstring(e) : std::wstring(L".");
}
