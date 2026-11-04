#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct UiState
{
    HWND hwnd = nullptr;
    HWND listbox = nullptr;
    HWND combo_model = nullptr;
    HWND combo_lang = nullptr;
    HWND combo_threads = nullptr;
    HWND chk_srt = nullptr;
    HWND chk_vad = nullptr;
    HWND chk_overwrite = nullptr;
    HWND chk_gpu = nullptr;
    HWND lbl_model = nullptr;
    HWND lbl_lang = nullptr;
    HWND lbl_threads = nullptr;
    HWND btn_add = nullptr;
    HWND btn_remove = nullptr;
    HWND btn_clear = nullptr;
    HWND btn_open_dir = nullptr;
    HWND btn_start = nullptr;
    HWND btn_cancel = nullptr;
    HWND progress = nullptr;
    HWND status_label = nullptr;
    HWND log = nullptr;
    HFONT font = nullptr;

    std::vector<std::wstring> files;
    std::wstring last_output_dir;
    bool busy = false;
};

void ui_create(UiState &ui, HINSTANCE instance, HWND parent);
void ui_layout(UiState &ui);
void ui_refresh_files(UiState &ui);
void ui_add_files(UiState &ui, const std::vector<std::wstring> &paths);
void ui_append_log(UiState &ui, const std::wstring &line);
void ui_set_progress(UiState &ui, int percent, const std::wstring &status);
void ui_set_busy(UiState &ui, bool busy);
