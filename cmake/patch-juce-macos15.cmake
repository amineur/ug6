# cmake/patch-juce-macos15.cmake
#
# Invoked via FetchContent_Declare(... PATCH_COMMAND cmake -DJUCE_SRC=<SOURCE_DIR> -P this_file).
# Stubs out CGWindowListCreateImage references in JUCE 7's juce_Windowing_mac.mm so the
# code compiles against the macOS 15 SDK (where Apple obsoleted that function).
# The replacement is a null function pointer cast — compiles fine, would crash if reached,
# but UG6 never triggers that JUCE code path (we have no GUI).

if(NOT DEFINED JUCE_SRC)
    message(WARNING "patch-juce-macos15.cmake invoked without JUCE_SRC")
    return()
endif()

set(target_file "${JUCE_SRC}/modules/juce_gui_basics/native/juce_Windowing_mac.mm")

if(NOT EXISTS "${target_file}")
    message(STATUS "[ug6] No juce_Windowing_mac.mm at ${target_file} — nothing to patch")
    return()
endif()

file(READ "${target_file}" content)

if(content MATCHES "UG6_STUB_CGWLCI")
    message(STATUS "[ug6] JUCE Windowing_mac.mm already patched")
    return()
endif()

if(NOT content MATCHES "CGWindowListCreateImage")
    message(STATUS "[ug6] No CGWindowListCreateImage symbol found — JUCE version may be different")
    return()
endif()

string(REPLACE
    "CGWindowListCreateImage"
    "((CGImageRef(*)(CGRect,CGWindowListOption,CGWindowID,CGWindowImageOption))NULL)/*UG6_STUB_CGWLCI*/"
    content "${content}")

file(WRITE "${target_file}" "${content}")
message(STATUS "[ug6] Patched JUCE Windowing_mac.mm for macOS 15 SDK")
