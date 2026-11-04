#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"
#include "util.h"

#include <cstdio>

bool decode_audio_16k_mono(const std::wstring &path, std::vector<float> &out, std::wstring &err)
{
    out.clear();

    const std::string utf8_path = u8(path);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 16000);
    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(utf8_path.c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS)
    {
        err = L"无法打开或解码音频文件 (miniaudio error " + std::to_wstring((int)result) + L")";
        return false;
    }

    const ma_uint64 chunk_frames = 8192;
    std::vector<float> buffer((size_t)chunk_frames);

    bool ok = true;
    for (;;)
    {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, buffer.data(), chunk_frames, &frames_read);
        if (result == MA_AT_END)
        {
            break;
        }
        if (result != MA_SUCCESS)
        {
            err = L"解码音频时出错 (miniaudio error " + std::to_wstring((int)result) + L")";
            ok = false;
            break;
        }
        if (frames_read == 0)
        {
            break;
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + (size_t)frames_read);
    }

    ma_decoder_uninit(&decoder);

    if (!ok)
    {
        out.clear();
        return false;
    }
    if (out.empty())
    {
        err = L"音频为空或无法解码 (可能不是受支持的 mp3/wav/flac)";
        return false;
    }
    return true;
}
