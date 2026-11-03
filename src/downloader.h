#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

using DownloadProgressCb = std::function<void(int64_t done, int64_t total)>;

bool http_download(const std::wstring & primary_url, const std::wstring & mirror_url, const std::wstring & dest_path,
                   std::atomic<bool> & cancel, const DownloadProgressCb & progress, std::wstring & err,
                   int64_t expected_size);
