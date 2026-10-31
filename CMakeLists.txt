cmake_minimum_required(VERSION 3.22)
project(FreesoundMini VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(juce)

juce_add_plugin(FreesoundMini
    COMPANY_NAME "PersonalLab"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    PLUGIN_MANUFACTURER_CODE PLab
    PLUGIN_CODE Fsmn
    FORMATS VST3 AU
    PRODUCT_NAME "FreesoundMini"
)

target_sources(FreesoundMini PRIVATE
    Source/PluginProcessor.cpp
)

target_compile_definitions(FreesoundMini PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(FreesoundMini PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_core
)
