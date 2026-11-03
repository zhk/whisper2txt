#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool decode_audio_16k_mono(const std::wstring & path, std::vector<float> & out, std::wstring & err);
