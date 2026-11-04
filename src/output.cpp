#include "output.h"
#include "util.h"

#include <fstream>

static const char kUtf8Bom[3] = {(char)0xEF, (char)0xBB, (char)0xBF};

bool write_txt(const std::wstring &path, const std::vector<Segment> &segments, std::wstring &err)
{
    const std::string narrow = u8(path);
    std::ofstream f(narrow.c_str(), std::ios::binary | std::ios::trunc);
    if (!f)
    {
        err = L"无法写入文件: " + path;
        return false;
    }

    f.write(kUtf8Bom, 3);
    for (const Segment &s : segments)
    {
        f.write(s.text.data(), (std::streamsize)s.text.size());
        f.write("\r\n", 2);
    }
    f.flush();
    if (!f)
    {
        err = L"写入文件失败: " + path;
        return false;
    }
    return true;
}

bool write_srt(const std::wstring &path, const std::vector<Segment> &segments, std::wstring &err)
{
    const std::string narrow = u8(path);
    std::ofstream f(narrow.c_str(), std::ios::binary | std::ios::trunc);
    if (!f)
    {
        err = L"无法写入文件: " + path;
        return false;
    }

    f.write(kUtf8Bom, 3);

    int index = 1;
    for (const Segment &s : segments)
    {
        std::wstring block;
        block += std::to_wstring(index++);
        block += L"\r\n";
        block += fmt_srt_time(s.t0_ms);
        block += L" --> ";
        block += fmt_srt_time(s.t1_ms);
        block += L"\r\n";
        block += wu(s.text);
        block += L"\r\n\r\n";
        const std::string out = u8(block);
        f.write(out.data(), (std::streamsize)out.size());
    }

    f.flush();
    if (!f)
    {
        err = L"写入文件失败: " + path;
        return false;
    }
    return true;
}
