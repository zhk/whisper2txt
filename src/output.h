#pragma once

#include <string>
#include <vector>

#include "engine.h"

bool write_txt(const std::wstring & path, const std::vector<Segment> & segments, std::wstring & err);
bool write_srt(const std::wstring & path, const std::vector<Segment> & segments, std::wstring & err);
