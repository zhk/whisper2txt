#pragma once

#include <cstdint>
#include <string>

struct ModelInfo
{
    const wchar_t *label;
    const wchar_t *filename;
    const wchar_t *url;
    const wchar_t *mirror_url;
    int64_t min_size;
    int64_t expected_size;
};

enum ModelSlot
{
    SLOT_TURBO = 0,
    SLOT_MEDIUM = 1,
    SLOT_LARGE = 2,
};

const ModelInfo &model_for_slot(int slot);
const ModelInfo &vad_model_info();

std::wstring models_dir();
std::wstring model_path(const ModelInfo &info);
bool model_ready(const ModelInfo &info);
