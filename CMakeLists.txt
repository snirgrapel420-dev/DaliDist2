cmake_minimum_required(VERSION 3.22)
project(DaliDist VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# If you already have JUCE locally (e.g. shared with the other Dali Audio plugins),
# replace the FetchContent block with: add_subdirectory(/path/to/JUCE JUCE)
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(DaliDist
    COMPANY_NAME                "Dali Audio"
    BUNDLE_ID                   com.daliaudio.dalidist
    PLUGIN_MANUFACTURER_CODE    Dali          # use the same code as the other Dali Audio plugins
    PLUGIN_CODE                 DDst
    FORMATS                     VST3 AU Standalone
    PRODUCT_NAME                "DaliDist"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Distortion
    AU_MAIN_TYPE                kAudioUnitType_Effect)

target_sources(DaliDist PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(DaliDist PRIVATE Source)

target_compile_definitions(DaliDist PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(DaliDist
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
