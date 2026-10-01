cmake_minimum_required(VERSION 3.22)

project(GuitarPedalboard VERSION 1.2.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Read sources as UTF-8 regardless of the Windows code page (e.g. 950 on zh-TW systems)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Link the C++ runtime statically, so the app runs on any Windows PC without installing
# the Visual C++ Redistributable (no "VCRUNTIME140.dll was not found" errors).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JUCE)

option(PEDALBOARD_BUILD_TESTS "Build the offline render / tuner test" ON)

# ---------------------------------------------------------------------------
# Plugin: VST3 + Standalone app
# ---------------------------------------------------------------------------
juce_add_plugin(GuitarPedalboard
    COMPANY_NAME "DIY Audio"
    BUNDLE_ID com.diyaudio.guitarpedalboard
    PLUGIN_MANUFACTURER_CODE Diya
    PLUGIN_CODE Gpb1
    FORMATS VST3 Standalone
    PRODUCT_NAME "Guitar Pedalboard"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Distortion
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(GuitarPedalboard
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/DSP/FxChain.cpp)

target_include_directories(GuitarPedalboard PRIVATE Source)

target_compile_definitions(GuitarPedalboard
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_ASIO=1                 # low-latency ASIO drivers in the standalone app
        JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(GuitarPedalboard
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline test: renders WAV files through every effect and checks the tuner
# ---------------------------------------------------------------------------
if(PEDALBOARD_BUILD_TESTS)
    juce_add_console_app(PedalTest PRODUCT_NAME "PedalTest")

    target_sources(PedalTest
        PRIVATE
            Tests/RenderTest.cpp
            Source/DSP/FxChain.cpp)

    target_include_directories(PedalTest PRIVATE Source)

    target_compile_definitions(PedalTest
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)

    target_link_libraries(PedalTest
        PRIVATE
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Renders the editor to a PNG (no window, no audio device), to check the UI layout
    juce_add_console_app(UiSnapshot PRODUCT_NAME "UiSnapshot")

    target_sources(UiSnapshot
        PRIVATE
            Tests/UiSnapshot.cpp
            Source/PluginProcessor.cpp
            Source/PluginEditor.cpp
            Source/DSP/FxChain.cpp)

    target_include_directories(UiSnapshot PRIVATE Source)

    target_compile_definitions(UiSnapshot
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JucePlugin_Name="Guitar Pedalboard")

    target_link_libraries(UiSnapshot
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
