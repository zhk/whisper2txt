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
    if (vctx_ != nullptr)
    {
        whisper_vad_free(vctx_);
        vctx_ = nullptr;
    }
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
    cparams.flash_attn = cfg_.use_gpu && cfg_.flash_attn;

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

    if (cfg_.vad)
    {
        const std::string vad_u8 = u8(cfg_.vad_model_path);
        if (vad_u8.empty())
        {
            log_line(L"未提供 VAD 模型路径，本次不使用 VAD");
        }
        else
        {
            vctx_ = whisper_vad_init_from_file_with_params(vad_u8.c_str(), whisper_vad_default_context_params());
            if (vctx_ == nullptr)
            {
                log_line(L"VAD 模型加载失败，本次不使用 VAD: " + cfg_.vad_model_path);
            }
            else
            {
                log_line(L"VAD 已启用 (按语音段切窗，静音不送识别): " + cfg_.vad_model_path);
            }
        }
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

        if (window_own_limit_ms_ > 0 && t0 - window_base_ms_ >= window_own_limit_ms_)
        {
            continue;
        }

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

    const bool high_quality = cfg_.quality == QualityMode::High;

    whisper_full_params params =
        whisper_full_default_params(high_quality ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    params.n_threads = cfg_.n_threads;
    params.strategy = high_quality ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY;
    params.greedy.best_of = high_quality ? 5 : 1;
    params.beam_search.beam_size = high_quality ? 5 : -1;
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
    params.temperature_inc = high_quality ? 0.4f : 0.2f;
    if (high_quality)
    {
        params.entropy_thold = 1.5f;
        params.logprob_thold = -1.2f;
    }
    if (cfg_.no_speech_thold > -50.0f)
    {
        params.no_speech_thold = cfg_.no_speech_thold;
    }
    params.new_segment_callback = &Transcriber::on_new_segment;
    params.new_segment_callback_user_data = this;
    params.progress_callback = &Transcriber::on_progress;
    params.progress_callback_user_data = this;
    params.abort_callback = &Transcriber::on_abort;
    params.abort_callback_user_data = this;

    if (!cfg_.initial_prompt.empty())
    {
        params.initial_prompt = cfg_.initial_prompt.c_str();
    }

    const whisper_vad_params vp = make_vad_params(high_quality);

    auto apply_language = [&](whisper_full_params &p)
    {
        switch (cfg_.lang)
        {
        case LangMode::Zh:
            p.language = "zh";
            break;
        case LangMode::En:
            p.language = "en";
            break;
        default:
            p.language = nullptr;
            break;
        }
        p.detect_language = false;
        p.offset_ms = 0;
        p.duration_ms = 0;
    };

    auto build_window_prompt = [&]()
    {
        window_prompt_.clear();
        if (!cfg_.initial_prompt.empty())
        {
            window_prompt_ = cfg_.initial_prompt;
        }
        if (sink_ != nullptr && !sink_->empty())
        {
            std::string tail;
            for (size_t i = sink_->size(); i > 0; --i)
            {
                const std::string &t = (*sink_)[i - 1].text;
                if (tail.size() + t.size() + 1 > 400)
                {
                    break;
                }
                tail = t + " " + tail;
            }
            if (!tail.empty())
            {
                if (!window_prompt_.empty())
                {
                    window_prompt_ += " ";
                }
                window_prompt_ += tail;
            }
        }
        if (window_prompt_.size() > 480)
        {
            size_t cut = window_prompt_.size() - 480;
            while (cut < window_prompt_.size() && ((unsigned char)window_prompt_[cut] & 0xC0) == 0x80)
            {
                ++cut;
            }
            window_prompt_ = window_prompt_.substr(cut);
        }
        params.initial_prompt = window_prompt_.empty() ? nullptr : window_prompt_.c_str();
    };

    if (vctx_ != nullptr)
    {
        std::vector<std::pair<int64_t, int64_t>> chunks;
        if (!vad_chunks(samples.data(), n_samples, vp, chunks, err))
        {
            return false;
        }
        if (chunks.empty())
        {
            log_line(L"VAD 未检测到语音段，输出为空（静音不会被强行识别）");
            if (progress_)
            {
                progress_(100, current_total_ms_, current_total_ms_);
            }
            return true;
        }

        int64_t covered_ms = 0;
        for (const auto &c : chunks)
        {
            covered_ms += c.second - c.first;
        }
        const int coverage = current_total_ms_ > 0 ? (int)(covered_ms * 100 / current_total_ms_) : 0;

        if (coverage >= 85)
        {
            log_line(L"VAD: 语音覆盖 " + std::to_wstring(coverage) + L"% → 整段连续识别（不切窗，保留完整上下文）");
        }
        else
        {
            log_line(L"VAD: 语音覆盖 " + std::to_wstring(coverage) + L"% → 按语音段切窗（跳过静音）");

            if (cfg_.lang == LangMode::Mixed)
            {
                last_language_.clear();
            }
            window_count_ = (int)chunks.size();
            int used = 0;
            int64_t sent_ms = 0;

            for (size_t ci = 0; ci < chunks.size(); ++ci)
            {
                if (cancel_.load())
                {
                    err = L"已取消";
                    return false;
                }

                const int64_t ms0 = chunks[ci].first;
                const int64_t ms1 = chunks[ci].second;
                const int64_t s0 = ms0 * WHISPER_SAMPLE_RATE / 1000;
                const int64_t s1 = std::min<int64_t>((int64_t)n_samples, ms1 * WHISPER_SAMPLE_RATE / 1000);
                if (s1 - s0 < WHISPER_SAMPLE_RATE / 5)
                {
                    continue;
                }

                sent_ms += (s1 - s0) * 1000 / WHISPER_SAMPLE_RATE;
                window_index_ = (int)ci;
                window_base_ms_ = ms0;
                window_own_limit_ms_ = 0;
                ++used;

                build_window_prompt();
                apply_language(params);

                const int ret = whisper_full_with_state(ctx_, state_, params, samples.data() + s0, (int)(s1 - s0));
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

                if (cfg_.lang == LangMode::Mixed)
                {
                    const int lang_id = whisper_full_lang_id_from_state(state_);
                    const char *lang = lang_id >= 0 ? whisper_lang_str(lang_id) : nullptr;
                    if (lang != nullptr)
                    {
                        last_language_ += lang;
                        last_language_ += ' ';
                    }
                }
                else
                {
                    const int lang_id = whisper_full_lang_id_from_state(state_);
                    const char *lang = lang_id >= 0 ? whisper_lang_str(lang_id) : nullptr;
                    last_language_ = lang != nullptr ? lang : "";
                }
            }

            log_line(L"VAD 切窗: " + std::to_wstring(used) + L" 块，实际送识别 " + std::to_wstring(sent_ms / 1000) +
                     L" s / 文件 " + std::to_wstring(current_total_ms_ / 1000) + L" s");

            dedupe_segments(out);
            apply_terms(out);

            if (progress_)
            {
                progress_(100, current_total_ms_, current_total_ms_);
            }
            return true;
        }
    }

    const int64_t win_samples = (int64_t)WHISPER_SAMPLE_RATE * 30;
    const int64_t hop_samples = (int64_t)WHISPER_SAMPLE_RATE * 28;
    const int64_t own_horizon_ms = 29000;

    if (cfg_.lang == LangMode::Mixed)
    {
        window_count_ = (int)(((int64_t)n_samples + hop_samples - 1) / hop_samples);
        if (window_count_ < 1)
        {
            window_count_ = 1;
        }
        last_language_.clear();

        int wi = 0;
        for (int64_t start = 0; start < n_samples; start += hop_samples, ++wi)
        {
            if (cancel_.load())
            {
                err = L"已取消";
                return false;
            }

            const int n = (int)std::min<int64_t>(win_samples, (int64_t)n_samples - start);
            const int64_t win_ms = (int64_t)n * 1000 / WHISPER_SAMPLE_RATE;

            window_index_ = wi;
            window_base_ms_ = start * 1000 / WHISPER_SAMPLE_RATE;
            window_own_limit_ms_ = std::min(own_horizon_ms, win_ms);

            build_window_prompt();
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

        dedupe_segments(out);
        apply_terms(out);

        if (progress_)
        {
            progress_(100, current_total_ms_, current_total_ms_);
        }
        return true;
    }

    window_count_ = 1;
    window_index_ = 0;
    window_base_ms_ = 0;
    window_own_limit_ms_ = 0;

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

    dedupe_segments(out);
    apply_terms(out);

    if (progress_)
    {
        progress_(100, current_total_ms_, current_total_ms_);
    }
    return true;
}

static bool ascii_iequals_ci(const std::string &s, size_t pos, const std::string &needle)
{
    if (pos + needle.size() > s.size())
    {
        return false;
    }
    for (size_t k = 0; k < needle.size(); ++k)
    {
        const unsigned char a = (unsigned char)s[pos + k];
        const unsigned char b = (unsigned char)needle[k];
        if (a == b)
        {
            continue;
        }
        const bool ascii_a = a < 0x80;
        const bool ascii_b = b < 0x80;
        if (!ascii_a || !ascii_b)
        {
            return false;
        }
        const unsigned char la = (a >= 'A' && a <= 'Z') ? (unsigned char)(a + 32) : a;
        const unsigned char lb = (b >= 'A' && b <= 'Z') ? (unsigned char)(b + 32) : b;
        if (la != lb)
        {
            return false;
        }
    }
    const size_t end = pos + needle.size();
    if (pos > 0)
    {
        const unsigned char prev = (unsigned char)s[pos - 1];
        if (prev >= '0' && prev <= '9')
        {
            return false;
        }
        if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z'))
        {
            if (!needle.empty())
            {
                const unsigned char first = (unsigned char)needle[0];
                if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9'))
                {
                    return false;
                }
            }
        }
    }
    if (end < s.size())
    {
        const unsigned char nxt = (unsigned char)s[end];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z') || (nxt >= '0' && nxt <= '9'))
        {
            if (!needle.empty())
            {
                const unsigned char last = (unsigned char)needle[needle.size() - 1];
                if ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9'))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static void replace_term_ci(std::string &text, const std::string &from, const std::string &to)
{
    if (from.empty() || from == to)
    {
        return;
    }
    std::string res;
    res.reserve(text.size());
    size_t i = 0;
    bool replaced = false;
    while (i < text.size())
    {
        if (ascii_iequals_ci(text, i, from))
        {
            res += to;
            i += from.size();
            replaced = true;
        }
        else
        {
            res += text[i];
            ++i;
        }
    }
    if (replaced)
    {
        text.swap(res);
    }
}

void Transcriber::apply_terms(std::vector<Segment> &segs) const
{
    if (cfg_.terms.empty())
    {
        return;
    }
    for (Segment &s : segs)
    {
        for (const auto &pair : cfg_.terms)
        {
            replace_term_ci(s.text, pair.first, pair.second);
        }
    }
}

static std::string norm_key(const std::string &text)
{
    std::string key;
    key.reserve(text.size());
    for (char c : text)
    {
        const unsigned char u = (unsigned char)c;
        if (u == ' ' || u == '\t' || u == '\r' || u == '\n')
        {
            continue;
        }
        if (u < 0x80)
        {
            if (u == ',' || u == '.' || u == '!' || u == '?' || u == ';' || u == ':' || u == '"' || u == '\'' ||
                u == '(' || u == ')')
            {
                continue;
            }
            if (u >= 'A' && u <= 'Z')
            {
                key += (char)(u + 32);
                continue;
            }
        }
        key += c;
    }
    return key;
}

void Transcriber::dedupe_segments(std::vector<Segment> &segs)
{
    if (segs.size() < 2)
    {
        return;
    }

    std::vector<Segment> kept;
    kept.reserve(segs.size());
    for (Segment &s : segs)
    {
        if (!kept.empty())
        {
            Segment &prev = kept.back();
            const std::string a = norm_key(prev.text);
            const std::string b = norm_key(s.text);
            if (!a.empty() && a == b)
            {
                if (s.t1_ms > prev.t1_ms)
                {
                    prev.t1_ms = s.t1_ms;
                }
                continue;
            }
            if (!a.empty() && !b.empty() && s.t0_ms < prev.t1_ms - 300)
            {
                if (b.size() >= a.size() && b.compare(0, a.size(), a) == 0)
                {
                    prev.text = s.text;
                    prev.t1_ms = s.t1_ms;
                    continue;
                }
                if (a.size() > b.size() && a.compare(0, b.size(), b) == 0)
                {
                    continue;
                }
            }
        }
        kept.push_back(std::move(s));
    }
    segs.swap(kept);
}

whisper_vad_params Transcriber::make_vad_params(bool high_quality) const
{
    whisper_vad_params vp = whisper_vad_default_params();
    if (high_quality)
    {
        vp.threshold = 0.35f;
        vp.speech_pad_ms = 150;
        vp.min_silence_duration_ms = 300;
        vp.max_speech_duration_s = 40.0f;
    }
    const VadTuning &vt = cfg_.vad_tuning;
    if (vt.threshold >= 0.0f)
    {
        vp.threshold = vt.threshold;
    }
    if (vt.min_speech_duration_ms >= 0)
    {
        vp.min_speech_duration_ms = vt.min_speech_duration_ms;
    }
    if (vt.min_silence_duration_ms >= 0)
    {
        vp.min_silence_duration_ms = vt.min_silence_duration_ms;
    }
    if (vt.max_speech_duration_s > 0.0f)
    {
        vp.max_speech_duration_s = vt.max_speech_duration_s;
    }
    if (vt.speech_pad_ms >= 0)
    {
        vp.speech_pad_ms = vt.speech_pad_ms;
    }
    return vp;
}

bool Transcriber::vad_chunks(const float *samples, int n_samples, const whisper_vad_params &vp,
    std::vector<std::pair<int64_t, int64_t>> &chunks, std::wstring &err)
{
    whisper_vad_segments *segs = whisper_vad_segments_from_samples(vctx_, vp, samples, n_samples);
    if (segs == nullptr)
    {
        err = L"VAD 检测失败 (whisper_vad_segments_from_samples 返回空)";
        return false;
    }

    const int n = whisper_vad_segments_n_segments(segs);
    const int64_t max_chunk_ms = 28000;
    const int64_t max_gap_ms = 1000;

    int64_t cur0 = -1;
    int64_t cur1 = -1;
    for (int i = 0; i < n; ++i)
    {
        if (cancel_.load())
        {
            whisper_vad_free_segments(segs);
            err = L"已取消";
            return false;
        }

        const int64_t s = (int64_t)(whisper_vad_segments_get_segment_t0(segs, i) * 10.0 + 0.5);
        const int64_t e = (int64_t)(whisper_vad_segments_get_segment_t1(segs, i) * 10.0 + 0.5);
        if (e <= s)
        {
            continue;
        }

        if (cur0 < 0)
        {
            cur0 = s;
            cur1 = e;
            continue;
        }
        if (e - cur0 <= max_chunk_ms && s - cur1 <= max_gap_ms)
        {
            cur1 = e;
            continue;
        }
        chunks.emplace_back(cur0, cur1);
        cur0 = s;
        cur1 = e;
    }
    if (cur0 >= 0)
    {
        chunks.emplace_back(cur0, cur1);
    }

    log_line(L"VAD 原始语音段: " + std::to_wstring(n) + L" 段" +
             (chunks.empty() ? L"" : L" -> 合并为 " + std::to_wstring(chunks.size()) + L" 块"));
    if (!chunks.empty())
    {
        wchar_t buf[128];
        swprintf(buf, 128, L"  首块 %.1f-%.1f s, 末块 %.1f-%.1f s", (double)chunks.front().first / 1000.0,
            (double)chunks.front().second / 1000.0, (double)chunks.back().first / 1000.0,
            (double)chunks.back().second / 1000.0);
        log_line(buf);
    }

    whisper_vad_free_segments(segs);
    return true;
}
