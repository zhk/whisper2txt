#include "models.h"
#include "util.h"

#include <windows.h>

static const wchar_t * kHf = L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
static const wchar_t * kHfMirror = L"https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/";

static ModelInfo kModels[3] = {
    {L"large-v3-turbo (推荐，中英混合最佳)", L"ggml-large-v3-turbo.bin", nullptr, nullptr, 1500000000LL, 1624555275LL},
    {L"medium (较快，中文良好)",             L"ggml-medium.bin",        nullptr, nullptr, 1400000000LL, 0},
    {L"large-v3 (最准，CPU 较慢)",           L"ggml-large-v3.bin",      nullptr, nullptr, 2900000000LL, 0},
};

static ModelInfo kVad = {
    L"Silero VAD", L"ggml-silero-v6.2.0.bin", nullptr, nullptr, 500000LL, 0,
};

static std::wstring g_model_urls[3][2];
static std::wstring g_vad_urls[2];

static void init_urls() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    for (int i = 0; i < 3; ++i) {
        g_model_urls[i][0] = std::wstring(kHf) + kModels[i].filename;
        g_model_urls[i][1] = std::wstring(kHfMirror) + kModels[i].filename;
        kModels[i].url = g_model_urls[i][0].c_str();
        kModels[i].mirror_url = g_model_urls[i][1].c_str();
    }

    g_vad_urls[0] = L"https://huggingface.co/ggml-org/whisper-vad/resolve/main/" + std::wstring(kVad.filename);
    g_vad_urls[1] = L"https://hf-mirror.com/ggml-org/whisper-vad/resolve/main/" + std::wstring(kVad.filename);
    kVad.url = g_vad_urls[0].c_str();
    kVad.mirror_url = g_vad_urls[1].c_str();
}

const ModelInfo & model_for_slot(int slot) {
    init_urls();
    if (slot < 0 || slot >= 3) {
        slot = SLOT_TURBO;
    }
    return kModels[slot];
}

const ModelInfo & vad_model_info() {
    init_urls();
    return kVad;
}

std::wstring models_dir() {
    std::wstring dir = local_appdata() + L"\\WhisperTxt\\models";
    CreateDirectoryW(dir.c_str(), nullptr);
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        std::wstring alt = std::wstring(local_appdata()) + L"\\WhisperTxt";
        CreateDirectoryW(alt.c_str(), nullptr);
        dir = alt + L"\\models";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring model_path(const ModelInfo & info) {
    return models_dir() + L"\\" + info.filename;
}

bool model_ready(const ModelInfo & info) {
    const std::wstring path = model_path(info);
    WIN32_FIND_DATAW find{};
    HANDLE h = FindFirstFileW(path.c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(h);
    LARGE_INTEGER li;
    li.HighPart = find.nFileSizeHigh;
    li.LowPart = find.nFileSizeLow;
    return li.QuadPart >= info.min_size;
}
