cmake_minimum_required(VERSION 3.22)

project(Limiter1 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(LIMITER1_DSP_OPTIONS /W4)
    set(LIMITER1_TEST_OPTIONS /W4)
else()
    set(LIMITER1_DSP_OPTIONS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    set(LIMITER1_TEST_OPTIONS -Wall -Wextra)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The limiter and re-quantizer have no JUCE dependency, so they can be tested offline.
add_library(limiter1_dsp STATIC
    Source/dsp/Limiter.cpp
    Source/dsp/Requantizer.cpp)
target_include_directories(limiter1_dsp PUBLIC Source)
target_compile_options(limiter1_dsp PRIVATE ${LIMITER1_DSP_OPTIONS})

# Tests: prints PASS/FAIL per check, non-zero exit on any failure.
add_executable(limiter1_test tests/limiter_test.cpp)
target_link_libraries(limiter1_test PRIVATE limiter1_dsp)
target_compile_options(limiter1_test PRIVATE ${LIMITER1_TEST_OPTIONS})
enable_testing()
add_test(NAME limiter_test COMMAND limiter1_test)

juce_add_plugin(Limiter1
    PRODUCT_NAME "Limiter1"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.limiter1"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Lim1
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics Mastering
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(Limiter1Assets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
set_target_properties(Limiter1Assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(Limiter1 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Limiter1 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Limiter1
    PRIVATE
        limiter1_dsp
        Limiter1Assets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline renderer through the exact processor path (see tools/render.cpp).
juce_add_console_app(limiter1_render PRODUCT_NAME "limiter1_render")
target_sources(limiter1_render PRIVATE
    tools/render.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)   # createEditor() is in the processor's vtable, so the editor must link
target_include_directories(limiter1_render PRIVATE Source)
target_compile_definitions(limiter1_render PRIVATE
    JucePlugin_Name="Limiter1"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(limiter1_render PRIVATE
    limiter1_dsp
    Limiter1Assets
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Renders the editor to a PNG with no host or screen (see tests/snapshot.cpp).
juce_add_console_app(limiter1_snapshot PRODUCT_NAME "limiter1_snapshot")
target_sources(limiter1_snapshot PRIVATE
    tests/snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_include_directories(limiter1_snapshot PRIVATE Source)
target_compile_definitions(limiter1_snapshot PRIVATE
    JucePlugin_Name="Limiter1"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(limiter1_snapshot PRIVATE
    limiter1_dsp
    Limiter1Assets
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
