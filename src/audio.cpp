#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"
#include "util.h"

#include <cmath>
#include <cstdio>

static double rms_db(const float *p, size_t n)
{
    if (n == 0)
    {
        return -120.0;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        acc += (double)p[i] * (double)p[i];
    }
    const double rms = std::sqrt(acc / (double)n);
    if (rms <= 1e-9)
    {
        return -120.0;
    }
    return 20.0 * std::log10(rms);
}

static double peak_db(const std::vector<float> &v)
{
    double peak = 0.0;
    for (float f : v)
    {
        const double a = std::fabs((double)f);
        if (a > peak)
        {
            peak = a;
        }
    }
    if (peak <= 1e-9)
    {
        return -120.0;
    }
    return 20.0 * std::log10(peak);
}

static void highpass_biquad(std::vector<float> &v, int hz, double sample_rate)
{
    const double w0 = 2.0 * 3.14159265358979323846 * (double)hz / sample_rate;
    const double q = 0.7071067811865476;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 + cosw) / 2.0 / a0;
    const double b1 = -(1.0 + cosw) / a0;
    const double b2 = b0;
    const double a1 = (-2.0 * cosw) / a0;
    const double a2 = (1.0 - alpha) / a0;

    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (float &f : v)
    {
        const double x0 = (double)f;
        double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        if (y0 > 1.0)
        {
            y0 = 1.0;
        }
        else if (y0 < -1.0)
        {
            y0 = -1.0;
        }
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        f = (float)y0;
    }
}

bool decode_audio_16k_mono(const std::wstring &path, std::vector<float> &out, const AudioPreproc &pp, AudioStats &stats,
    std::wstring &err)
{
    out.clear();
    stats = AudioStats();

    const std::string utf8_path = u8(path);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 16000);
    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(utf8_path.c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS)
    {
        err = L"无法打开或解码音频文件 (miniaudio error " + std::to_wstring((int)result) + L")";
        return false;
    }

    ma_format fmt = ma_format_f32;
    ma_uint32 channels = 1;
    ma_uint32 rate = 16000;
    if (ma_decoder_get_data_format(&decoder, &fmt, &channels, &rate, nullptr, 0) != MA_SUCCESS || channels == 0)
    {
        channels = 1;
    }
    stats.channels = (int)channels;

    const ma_uint64 chunk_frames = 8192;
    std::vector<float> buffer((size_t)chunk_frames * channels);
    std::vector<double> energy((size_t)channels, 0.0);
    uint64_t frames_total = 0;

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

        for (ma_uint64 fr = 0; fr < frames_read; ++fr)
        {
            for (ma_uint32 ch = 0; ch < channels; ++ch)
            {
                const double s = (double)buffer[(size_t)fr * channels + ch];
                energy[(size_t)ch] += s * s;
            }
        }

        out.insert(out.end(), buffer.begin(), buffer.begin() + (size_t)frames_read * channels);
        frames_total += frames_read;
    }

    ma_decoder_uninit(&decoder);

    if (!ok)
    {
        out.clear();
        return false;
    }
    if (out.empty() || frames_total == 0)
    {
        out.clear();
        err = L"音频为空或无法解码 (可能不是受支持的 mp3/wav/flac)";
        return false;
    }

    if (channels > 1)
    {
        int picked = 0;
        if (pp.select_loudest_channel)
        {
            double best = -1.0;
            for (size_t ch = 0; ch < energy.size(); ++ch)
            {
                if (energy[ch] > best)
                {
                    best = energy[ch];
                    picked = (int)ch;
                }
            }
        }

        stats.picked_channel = picked;

        std::vector<float> mono((size_t)frames_total);
        for (uint64_t fr = 0; fr < frames_total; ++fr)
        {
            if (pp.select_loudest_channel)
            {
                mono[(size_t)fr] = out[(size_t)fr * channels + (size_t)picked];
            }
            else
            {
                double acc = 0.0;
                for (ma_uint32 ch = 0; ch < channels; ++ch)
                {
                    acc += (double)out[(size_t)fr * channels + ch];
                }
                mono[(size_t)fr] = (float)(acc / (double)channels);
            }
        }
        out.swap(mono);
    }

    if (pp.highpass_hz >= 20 && pp.highpass_hz <= 7000)
    {
        highpass_biquad(out, pp.highpass_hz, 16000.0);
        stats.highpassed = true;
    }

    stats.rms_before_db = rms_db(out.data(), out.size());
    stats.peak_before_db = peak_db(out);

    if (pp.normalize)
    {
        double gain = 1.0;
        bool applied = false;
        bool rms_based = false;

        if (pp.loudnorm && stats.rms_before_db > -60.0)
        {
            const double target_rms = 0.1;
            double rms = 0.0;
            for (float f : out)
            {
                rms += (double)f * (double)f;
            }
            rms = std::sqrt(rms / (double)out.size());
            if (rms > 1e-6)
            {
                gain = target_rms / rms;
                if (gain > 16.0)
                {
                    gain = 16.0;
                }
                else if (gain < 0.0625)
                {
                    gain = 0.0625;
                }
                rms_based = true;
                applied = true;
            }
        }

        if (!rms_based)
        {
            double peak = 0.0;
            for (float f : out)
            {
                const double a = std::fabs((double)f);
                if (a > peak)
                    peak = a;
            }
            if (peak > 1e-6)
            {
                gain = 0.891 / peak;
                if (gain > 16.0)
                {
                    gain = 16.0;
                }
                else if (gain < 0.0625)
                {
                    gain = 0.0625;
                }
                applied = true;
            }
        }

        if (applied && (gain < 0.98 || gain > 1.02))
        {
            for (float &f : out)
            {
                double v = (double)f * gain;
                if (v > 1.0)
                    v = 1.0;
                else if (v < -1.0)
                    v = -1.0;
                f = (float)v;
            }
            if (rms_based)
            {
                double peak_after = 0.0;
                for (float f : out)
                {
                    const double a = std::fabs((double)f);
                    if (a > peak_after)
                        peak_after = a;
                }
                if (peak_after > 0.98)
                {
                    const double limiter_gain = 0.98 / peak_after;
                    for (float &f : out)
                    {
                        f = (float)((double)f * limiter_gain);
                    }
                    gain *= limiter_gain;
                }
            }
            stats.normalized = true;
            stats.loudnormed = rms_based;
            stats.gain_db = 20.0 * std::log10(gain);
        }
    }

    stats.rms_after_db = rms_db(out.data(), out.size());
    stats.peak_after_db = peak_db(out);

    return true;
}
