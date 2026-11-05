#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AudioPreproc
{
    bool select_loudest_channel = true;
    bool normalize = true;
    int highpass_hz = 0;
};

struct AudioStats
{
    int channels = 1;
    int picked_channel = 0;
    double peak_before_db = 0.0;
    double peak_after_db = 0.0;
    double gain_db = 0.0;
    bool normalized = false;
    bool highpassed = false;
};

bool decode_audio_16k_mono(const std::wstring &path, std::vector<float> &out, const AudioPreproc &pp, AudioStats &stats,
    std::wstring &err);
