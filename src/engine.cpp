#include "engine.h"
#include "util.h"

#include <ggml.h>
#include <ggml-backend.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>

static void whisper_log_bridge(enum ggml_log_level level, const char *text, void *user_data)
{
    if (level > GGML_LOG_LEVEL_WARN)
    {
        return;
    }
    auto *self = static_cast<Transcriber *>(user_data);
    if (self == nullptr || text == nullptr)
    {
        return;
    }
    std::string line(text);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    {
        line.pop_back();
    }
    if (line.empty() || line.find("system_info") != std::string::npos)
    {
        return;
    }
    self->log_line(wu(line));
}

static std::wstring gpu_device_list()
{
    std::wstring names;
    const size_t count = ggml_backend_dev_count();
    for (size_t i = 0; i < count; ++i)
    {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type dtype = ggml_backend_dev_type(dev);
        if (dtype != GGML_BACKEND_DEVICE_TYPE_GPU && dtype != GGML_BACKEND_DEVICE_TYPE_IGPU)
        {
            continue;
        }
        if (!names.empty())
        {
            names += L", ";
        }
        const char *name = ggml_backend_dev_name(dev);
        names += (name != nullptr) ? wu(name) : L"?";
    }
    return names;
}

static int physical_core_count()
{
    const int logical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    int info[4] = {0};
    __cpuid(info, 1);
    const bool hyperthreading = (info[3] & (1 << 28)) != 0;
    int physical = hyperthreading && logical >= 2 ? logical / 2 : logical;
    if (physical < 1)
    {
        physical = 1;
    }
    return physical;
}

Transcriber::~Transcriber()
{
    if (ctx_ != nullptr)
    {
        whisper_free(ctx_);
        ctx_ = nullptr;
        state_ = nullptr;
    }
}

void Transcriber::log_line(const std::wstring &line)
{
    if (log_)
    {
        log_(line);
    }
}

bool Transcriber::load(const EngineConfig &cfg, const LogFn &log, std::wstring &err)
{
    cfg_ = cfg;
    log_ = log;

    whisper_log_set(&whisper_log_bridge, this);

    static std::once_flag backend_load_once;
    std::call_once(backend_load_once, []
        { ggml_backend_load_all(); });

    if (cfg_.n_threads <= 0)
    {
        cfg_.n_threads = physical_core_count();
    }

    if (cfg_.use_gpu)
    {
        const std::wstring gpus = gpu_device_list();
        if (gpus.empty())
        {
            log_line(L"已勾选 GPU 加速，但未发现可用 GPU 设备 (缺少 ggml-cuda/ggml-vulkan 后端或驱动)，改用 CPU");
        }
        else
        {
            log_line(L"GPU 加速已启用，可用设备: " + gpus);
        }
    }
    else
    {
        log_line(L"未勾选 GPU 加速，使用 CPU 计算");
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = cfg_.use_gpu;
    cparams.flash_attn = false;

    const std::string model_u8 = u8(cfg_.model_path);
    ctx_ = whisper_init_from_file_with_params(model_u8.c_str(), cparams);
    if (ctx_ == nullptr)
    {
        err = L"加载模型失败: " + cfg_.model_path;
        return false;
    }

    state_ = whisper_init_state(ctx_);
    if (state_ == nullptr)
    {
        err = L"初始化 whisper state 失败";
        return false;
    }

    if (whisper_is_multilingual(ctx_) == 0 && cfg_.lang != LangMode::En)
    {
        err = L"该模型是纯英文模型，不能转写中文，请选择多语言模型 (如 ggml-large-v3-turbo)";
        return false;
    }

    return true;
}

void Transcriber::request_cancel()
{
    cancel_.store(true);
}

void Transcriber::reset_cancel()
{
    cancel_.store(false);
}

bool Transcriber::on_abort(void *user_data)
{
    auto *self = static_cast<Transcriber *>(user_data);
    return self->cancel_.load();
}

void Transcriber::on_new_segment(struct whisper_context *ctx, struct whisper_state *state, int n_new, void *user_data)
{
    (void)ctx;
    auto *self = static_cast<Transcriber *>(user_data);
    self->emit_segments(state, n_new);
}

void Transcriber::emit_segments(struct whisper_state *state, int n_new)
{
    if (sink_ == nullptr)
    {
        return;
    }
    const int total = whisper_full_n_segments_from_state(state);
    for (int i = total - n_new; i < total; ++i)
    {
        int64_t t0 = whisper_full_get_segment_t0_from_state(state, i) * 10 + window_base_ms_;
        int64_t t1 = whisper_full_get_segment_t1_from_state(state, i) * 10 + window_base_ms_;

        const char *raw = whisper_full_get_segment_text_from_state(state, i);
        if (raw == nullptr)
        {
            continue;
        }

        std::string text(raw);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        {
            text.erase(text.begin());
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        {
            text.pop_back();
        }
        if (text.empty() || text == "[BLANK_AUDIO]" || text == "(blank audio)")
        {
            continue;
        }

        if (t0 < 0)
        {
            t0 = 0;
        }
        if (t1 < t0)
        {
            t1 = t0;
        }

        Segment s;
        s.t0_ms = t0;
        s.t1_ms = t1;
        s.text = text;
        sink_->push_back(std::move(s));
    }
}

void Transcriber::on_progress(struct whisper_context *ctx, struct whisper_state *state, int progress, void *user_data)
{
    (void)ctx;
    (void)state;
    auto *self = static_cast<Transcriber *>(user_data);
    if (!self->progress_)
    {
        return;
    }
    int percent = progress;
    if (self->window_count_ > 1)
    {
        percent = (self->window_index_ * 100 + progress) / self->window_count_;
    }
    if (percent < 0)
    {
        percent = 0;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    const int64_t done_ms = self->current_total_ms_ * percent / 100;
    self->progress_(percent, done_ms, self->current_total_ms_);
}

bool Transcriber::transcribe(const std::vector<float> &samples, std::vector<Segment> &out, const ProgressFn &progress,
    const LogFn &log, std::wstring &err)
{
    if (ctx_ == nullptr || state_ == nullptr)
    {
        err = L"引擎尚未加载模型";
        return false;
    }

    sink_ = &out;
    progress_ = progress;
    log_ = log;
    out.clear();

    const int n_samples = (int)samples.size();
    current_total_ms_ = (int64_t)n_samples * 1000 / WHISPER_SAMPLE_RATE;

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = cfg_.n_threads;
    params.strategy = WHISPER_SAMPLING_GREEDY;
    params.greedy.best_of = 1;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.no_timestamps = false;
    params.single_segment = false;
    params.translate = false;
    params.no_context = false;
    params.suppress_blank = true;
    params.suppress_nst = true;
    params.token_timestamps = false;
    params.max_len = 0;
    params.temperature_inc = 0.2f;
    params.new_segment_callback = &Transcriber::on_new_segment;
    params.new_segment_callback_user_data = this;
    params.progress_callback = &Transcriber::on_progress;
    params.progress_callback_user_data = this;
    params.abort_callback = &Transcriber::on_abort;
    params.abort_callback_user_data = this;

    const std::string vad_path = cfg_.vad ? u8(cfg_.vad_model_path) : std::string();
    if (cfg_.vad && !vad_path.empty())
    {
        params.vad = true;
        params.vad_model_path = vad_path.c_str();
        params.vad_params = whisper_vad_default_params();
    }

    const int64_t win_samples = (int64_t)WHISPER_SAMPLE_RATE * 30;

    if (cfg_.lang == LangMode::Mixed)
    {
        window_count_ = (int)(((int64_t)n_samples + win_samples - 1) / win_samples);
        if (window_count_ < 1)
        {
            window_count_ = 1;
        }
        last_language_.clear();

        std::vector<float> detected((size_t)whisper_lang_max_id() + 1, 0.0f);
        (void)detected;

        for (int64_t start = 0; start < n_samples; start += win_samples)
        {
            if (cancel_.load())
            {
                err = L"已取消";
                return false;
            }

            const int n = (int)std::min<int64_t>(win_samples, (int64_t)n_samples - start);

            window_index_ = (int)(start / win_samples);
            window_base_ms_ = start * 1000 / WHISPER_SAMPLE_RATE;

            params.language = nullptr;
            params.detect_language = false;
            params.offset_ms = 0;
            params.duration_ms = 0;

            const int ret = whisper_full_with_state(ctx_, state_, params, samples.data() + start, n);
            if (cancel_.load())
            {
                err = L"已取消";
                return false;
            }
            if (ret != 0)
            {
                err = L"转写失败 (whisper_full 返回 " + std::to_wstring(ret) + L")";
                return false;
            }

            const int lang_id = whisper_full_lang_id_from_state(state_);
            const char *lang = lang_id >= 0 ? whisper_lang_str(lang_id) : nullptr;
            if (lang != nullptr)
            {
                last_language_ += lang;
                last_language_ += ' ';
            }
        }

        if (progress_)
        {
            progress_(100, current_total_ms_, current_total_ms_);
        }
        return true;
    }

    window_count_ = 1;
    window_index_ = 0;
    window_base_ms_ = 0;

    switch (cfg_.lang)
    {
    case LangMode::Zh:
        params.language = "zh";
        break;
    case LangMode::En:
        params.language = "en";
        break;
    default:
        params.language = nullptr;
        break;
    }
    params.detect_language = false;

    const int ret = whisper_full_with_state(ctx_, state_, params, samples.data(), n_samples);
    if (cancel_.load())
    {
        err = L"已取消";
        return false;
    }
    if (ret != 0)
    {
        err = L"转写失败 (whisper_full 返回 " + std::to_wstring(ret) + L")";
        return false;
    }

    const int lang_id = whisper_full_lang_id_from_state(state_);
    const char *lang = lang_id >= 0 ? whisper_lang_str(lang_id) : nullptr;
    last_language_ = lang != nullptr ? lang : "";

    if (progress_)
    {
        progress_(100, current_total_ms_, current_total_ms_);
    }
    return true;
}
