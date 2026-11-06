cmake_minimum_required(VERSION 3.16)
project(whisper2txt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP ON CACHE BOOL "" FORCE)

option(WHISPER2TXT_SHARED "build whisper/ggml as DLLs with dynamically loaded backends" ON)
if(WHISPER2TXT_SHARED)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(GGML_BACKEND_DL ON CACHE BOOL "" FORCE)
else()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(third_party/whisper.cpp)

add_executable(whisper2txt WIN32
    src/main.cpp
    src/ui.cpp
    src/engine.cpp
    src/audio.cpp
    src/output.cpp
    src/downloader.cpp
    src/models.cpp
    src/app.rc
)

target_include_directories(whisper2txt PRIVATE
    src
    third_party/whisper.cpp/include
    third_party/whisper.cpp/examples
)

target_link_libraries(whisper2txt PRIVATE
    whisper
    comctl32
    comdlg32
    shell32
    ole32
    winhttp
)

target_compile_definitions(whisper2txt PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    target_compile_options(whisper2txt PRIVATE /utf-8 /W3)
    target_link_options(whisper2txt PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    )
endif()
