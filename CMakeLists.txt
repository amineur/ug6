cmake_minimum_required(VERSION 3.22)

project(UG6Broadcaster
    VERSION 0.1.0
    DESCRIPTION "UG6 Broadcast audio processor + Icecast encoder"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS deployment target — 10.13 = High Sierra (Intel + Rosetta compat),
# Apple Silicon binaries still build fine targeting this.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies — pulled at configure time via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# JUCE 7.0.12 — last 7.x release; preserves Win7 SP1 compatibility on Windows,
# and compiles fine on macOS 14 (GitHub Actions macos-latest runner).
# Known caveat : doesn't compile on macOS 15 (Sequoia) locally because Apple
# removed CGWindowListCreateImage from the SDK. We accept that for now — local
# dev on Mac 15 is not a priority compared to producing a Windows binary via CI.
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        7.0.12
    GIT_SHALLOW    TRUE
)

# cpp-httplib — single-header HTTP server, supports SSE via chunked content provider.
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE httplib)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
juce_add_console_app(UG6Broadcaster
    PRODUCT_NAME  "UG6 Broadcaster"
    COMPANY_NAME  "UG6"
    BUNDLE_ID     "com.ug6.broadcaster"
)

juce_generate_juce_header(UG6Broadcaster)

# Embed web assets (HTML/CSS/JS) directly into the binary so we ship a single file.
juce_add_binary_data(UG6BroadcasterAssets
    HEADER_NAME WebAssets.h
    NAMESPACE   WebAssets
    SOURCES
        ui/index.html
)

target_sources(UG6Broadcaster PRIVATE
    src/Main.cpp
    src/AudioEngine.cpp
    src/BroadcastSender.cpp
    src/DspChain.cpp
    src/FilePlayer.cpp
    src/HttpServer.cpp
    src/InstanceConfig.cpp
    src/Presets.cpp
)

# ---------------------------------------------------------------------------
#  Optional encoder / streaming deps
#  On macOS install via Homebrew: brew install lame libshout
#  If missing, the build still works — BroadcastSender becomes a no-op stub.
# ---------------------------------------------------------------------------
set(UG6_BREW_HINTS /opt/homebrew /usr/local /opt/local)

find_path(LAME_INCLUDE_DIR lame/lame.h
    HINTS ${UG6_BREW_HINTS} PATH_SUFFIXES include)
find_library(LAME_LIBRARY mp3lame
    HINTS ${UG6_BREW_HINTS} PATH_SUFFIXES lib)

find_path(SHOUT_INCLUDE_DIR shout/shout.h
    HINTS ${UG6_BREW_HINTS} PATH_SUFFIXES include)
find_library(SHOUT_LIBRARY shout
    HINTS ${UG6_BREW_HINTS} PATH_SUFFIXES lib)

if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    message(STATUS "LAME found: ${LAME_LIBRARY}")
    target_compile_definitions(UG6Broadcaster PRIVATE UG6_HAS_LAME=1)
    target_include_directories(UG6Broadcaster PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(UG6Broadcaster PRIVATE ${LAME_LIBRARY})
else()
    message(WARNING "LAME not found — MP3 encoder will be disabled.\n  macOS : brew install lame")
    target_compile_definitions(UG6Broadcaster PRIVATE UG6_HAS_LAME=0)
endif()

if(SHOUT_INCLUDE_DIR AND SHOUT_LIBRARY)
    message(STATUS "libshout found: ${SHOUT_LIBRARY}")
    target_compile_definitions(UG6Broadcaster PRIVATE UG6_HAS_SHOUT=1)
    target_include_directories(UG6Broadcaster PRIVATE ${SHOUT_INCLUDE_DIR})
    target_link_libraries(UG6Broadcaster PRIVATE ${SHOUT_LIBRARY})
else()
    message(WARNING "libshout not found — Icecast push will be disabled.\n  macOS : brew install libshout")
    target_compile_definitions(UG6Broadcaster PRIVATE UG6_HAS_SHOUT=0)
endif()

target_include_directories(UG6Broadcaster PRIVATE src)

target_compile_definitions(UG6Broadcaster PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_MP3AUDIOFORMAT=1          # MP3 decode for the FilePlayer
)

target_link_libraries(UG6Broadcaster PRIVATE
    UG6BroadcasterAssets
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_gui_basics          # required for ScopedJuceInitialiser_GUI / message manager
    httplib::httplib
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# Platform tweaks
# ---------------------------------------------------------------------------
if(WIN32)
    target_compile_definitions(UG6Broadcaster PRIVATE
        _WIN32_WINNT=0x0601    # Windows 7 SP1
        WINVER=0x0601
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    # cpp-httplib pragma-links ws2_32 on MSVC; nothing else needed.
endif()

if(APPLE)
    set_target_properties(UG6Broadcaster PROPERTIES
        XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++17"
        XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
    )
endif()
