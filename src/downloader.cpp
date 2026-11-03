#include "downloader.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

struct UrlParts {
    std::wstring host;
    std::wstring path;
    bool https = true;
    bool valid = false;
};

UrlParts parse_url(const std::wstring & url) {
    UrlParts parts;
    const std::wstring scheme_https = L"https://";
    const std::wstring scheme_http = L"http://";

    std::wstring rest;
    if (url.rfind(scheme_https, 0) == 0) {
        parts.https = true;
        rest = url.substr(scheme_https.size());
    } else if (url.rfind(scheme_http, 0) == 0) {
        parts.https = false;
        rest = url.substr(scheme_http.size());
    } else {
        return parts;
    }

    const size_t slash = rest.find(L'/');
    if (slash == std::wstring::npos) {
        parts.host = rest;
        parts.path = L"/";
    } else {
        parts.host = rest.substr(0, slash);
        parts.path = rest.substr(slash);
    }
    parts.valid = !parts.host.empty();
    return parts;
}

bool query_content_length(HINTERNET request, int64_t & total) {
    total = -1;

    wchar_t length[64] = {0};
    DWORD size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                           length, &size, WINHTTP_NO_HEADER_INDEX)) {
        total = (int64_t) _wtoi64(length);
    }

    if (total <= 0) {
        wchar_t buffer[64] = {0};
        DWORD buffer_size = sizeof(buffer);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"x-linked-size", buffer, &buffer_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            total = (int64_t) _wtoi64(buffer);
        }
    }

    return total > 0;
}

bool download_from_url(const std::wstring & url, const std::wstring & dest_path, std::atomic<bool> & cancel,
                       const DownloadProgressCb & progress, std::wstring & err, bool & network_error,
                       int64_t expected_size) {
    network_error = false;

    const UrlParts parts = parse_url(url);
    if (!parts.valid) {
        err = L"无效的下载 URL";
        return false;
    }

    const std::wstring part_path = dest_path + L".part";

    HINTERNET session = WinHttpOpen(L"WhisperTxt/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        err = L"WinHttpOpen 失败";
        network_error = true;
        return false;
    }

    WinHttpSetTimeouts(session, 20000, 20000, 60000, 300000);

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    const INTERNET_PORT port = parts.https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(), port, 0);
    if (connect == nullptr) {
        err = L"无法连接服务器: " + parts.host;
        network_error = true;
        WinHttpCloseHandle(session);
        return false;
    }

    const DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;

    int64_t existing = 0;
    {
        WIN32_FIND_DATAW find{};
        HANDLE hfind = FindFirstFileW(part_path.c_str(), &find);
        if (hfind != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.HighPart = find.nFileSizeHigh;
            li.LowPart = find.nFileSizeLow;
            existing = li.QuadPart;
            FindClose(hfind);
        }
    }

    bool success = false;
    int64_t total = -1;

    for (int attempt = 0; attempt < 10 && !success; ++attempt) {
        if (cancel.load()) {
            err = L"下载已取消";
            break;
        }

        const bool resume = existing > 0;

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (request == nullptr) {
            err = L"WinHttpOpenRequest 失败";
            network_error = true;
            break;
        }

        if (resume) {
            const std::wstring range = L"Range: bytes=" + std::to_wstring(existing) + L"-\r\n";
            if (!WinHttpAddRequestHeaders(request, range.c_str(), (DWORD) -1, WINHTTP_ADDREQ_FLAG_ADD)) {
                WinHttpCloseHandle(request);
                break;
            }
        }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            const DWORD code = GetLastError();
            err = L"网络请求失败 (code " + std::to_wstring(code) + L")";
            network_error = true;
            WinHttpCloseHandle(request);
            Sleep(1000);
            continue;
        }

        DWORD status_size = sizeof(int);
        int status_code = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);

        if (status_code == 416) {
            existing = 0;
            DeleteFileW(part_path.c_str());
            WinHttpCloseHandle(request);
            continue;
        }

        if (status_code != 200 && status_code != 206) {
            err = L"服务器返回 HTTP " + std::to_wstring(status_code);
            WinHttpCloseHandle(request);
            break;
        }

        int64_t body_len = -1;
        query_content_length(request, body_len);

        if (status_code == 206) {
            total = body_len > 0 ? existing + body_len : (expected_size > 0 ? expected_size : -1);
        } else {
            if (resume) {
                existing = 0;
                DeleteFileW(part_path.c_str());
            }
            total = body_len > 0 ? body_len : (expected_size > 0 ? expected_size : -1);
        }

        HANDLE file = CreateFileW(part_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  existing > 0 ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err = L"无法创建文件: " + part_path;
            WinHttpCloseHandle(request);
            break;
        }

        {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            SetFilePointerEx(file, zero, nullptr, FILE_END);
        }

        std::vector<char> buffer(256 * 1024);
        int64_t done = existing;
        int64_t last_report = done;
        bool io_error = false;
        bool aborted = false;
        bool too_slow = false;

        auto speed_clock = std::chrono::steady_clock::now();
        int64_t speed_base = done;

        for (;;) {
            if (cancel.load()) {
                err = L"下载已取消";
                aborted = true;
                break;
            }

            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) {
                break;
            }

            const DWORD to_read = avail < (DWORD) buffer.size() ? avail : (DWORD) buffer.size();
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), to_read, &read)) {
                err = L"读取下载数据失败";
                network_error = true;
                io_error = true;
                break;
            }
            if (read == 0) {
                break;
            }

            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
                err = L"写入下载文件失败";
                io_error = true;
                break;
            }

            done += (int64_t) read;
            if (progress && (done - last_report >= 4 * 1024 * 1024 || done == total)) {
                last_report = done;
                progress(done, total);
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - speed_clock).count();
            if (elapsed > 15.0) {
                const double mbps = (double) (done - speed_base) / elapsed / (1024.0 * 1024.0);
                if (mbps < 0.10) {
                    too_slow = true;
                    break;
                }
                speed_clock = now;
                speed_base = done;
            }
        }

        CloseHandle(file);
        WinHttpCloseHandle(request);

        if (too_slow) {
            existing = done;
            err = L"下载速度过慢";
            network_error = true;
            break;
        }
        if (aborted) {
            break;
        }
        if (io_error) {
            existing = done;
            Sleep(1000);
            continue;
        }

        if (total > 0 && done != total) {
            existing = done;
            err.clear();
            Sleep(1000);
            continue;
        }

        if (progress) {
            progress(done, total > 0 ? total : done);
        }
        success = true;
    }

    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!success) {
        return false;
    }

    if (!MoveFileExW(part_path.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"重命名下载文件失败";
        return false;
    }

    return true;
}

} // namespace

bool http_download(const std::wstring & primary_url, const std::wstring & mirror_url, const std::wstring & dest_path,
                   std::atomic<bool> & cancel, const DownloadProgressCb & progress, std::wstring & err,
                   int64_t expected_size) {
    bool network_error = false;
    err.clear();

    if (download_from_url(primary_url, dest_path, cancel, progress, err, network_error, expected_size)) {
        return true;
    }

    if (cancel.load()) {
        return false;
    }

    if (!mirror_url.empty()) {
        std::wstring mirror_err;
        bool mirror_net_error = false;
        if (download_from_url(mirror_url, dest_path, cancel, progress, mirror_err, mirror_net_error, expected_size)) {
            return true;
        }
        if (!mirror_err.empty()) {
            err = err + L" | 镜像: " + mirror_err;
        }
    }

    return false;
}
