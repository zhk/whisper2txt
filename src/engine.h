#pragma once

#include <whisper.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Segment
{
    int64_t t0_ms;
    int64_t t1_ms;
    std::string text;
};

enum class LangMode
{
    Zh,
    En,
    Auto,
    Mixed,
};

enum class QualityMode
{
    Fast,
    High,
};

struct VadTuning
{
    float threshold = -1.0f;
    int min_speech_duration_ms = -1;
    int min_silence_duration_ms = -1;
    float max_speech_duration_s = -1.0f;
    int speech_pad_ms = -1;
};

struct EngineConfig
{
    std::wstring model_path;
    std::wstring vad_model_path;
    int n_threads = 0;
    LangMode lang = LangMode::Zh;
    bool vad = false;
    bool use_gpu = true;
    bool flash_attn = false;
    QualityMode quality = QualityMode::Fast;
    std::string initial_prompt;
    std::vector<std::pair<std::string, std::string>> terms;
    VadTuning vad_tuning;
};

class Transcriber
{
public:
    using LogFn = std::function<void(const std::wstring &)>;
    using ProgressFn = std::function<void(int percent, int64_t done_ms, int64_t total_ms)>;

    ~Transcriber();

    bool load(const EngineConfig &cfg, const LogFn &log, std::wstring &err);
    bool transcribe(const std::vector<float> &samples, std::vector<Segment> &out, const ProgressFn &progress,
        const LogFn &log, std::wstring &err);
    void request_cancel();
    void reset_cancel();
    void log_line(const std::wstring &line);

    std::string last_language() const
    {
        return last_language_;
    }

private:
    static void on_new_segment(struct whisper_context *ctx, struct whisper_state *state, int n_new, void *user_data);
    static void on_progress(struct whisper_context *ctx, struct whisper_state *state, int progress, void *user_data);
    static bool on_abort(void *user_data);

    void emit_segments(struct whisper_state *state, int n_new);
    void apply_terms(std::vector<Segment> &segs) const;
    static void dedupe_segments(std::vector<Segment> &segs);

    struct whisper_context *ctx_ = nullptr;
    struct whisper_state *state_ = nullptr;
    EngineConfig cfg_;
    std::atomic<bool> cancel_{false};

    std::vector<Segment> *sink_ = nullptr;
    ProgressFn progress_;
    LogFn log_;

    int window_index_ = 0;
    int window_count_ = 1;
    int64_t window_base_ms_ = 0;
    int64_t window_own_limit_ms_ = 0;
    int64_t current_total_ms_ = 0;
    std::string window_prompt_;

    std::string last_language_;
};
