cmake_minimum_required(VERSION 3.22)

# macOS: binario universal (Intel + Apple Silicon). Debe definirse antes de project().
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Arquitecturas de macOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Version minima de macOS")
endif()

project(KickClone VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows: runtime de C++ enlazado de forma estatica (el usuario no necesita instalar nada).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(KICK_BUILD_TESTS "Compilar las pruebas del nucleo DSP" OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.8
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(KickClone
    COMPANY_NAME "Gallogit91"
    PLUGIN_MANUFACTURER_CODE Glgt
    PLUGIN_CODE Kclo
    FORMATS VST3
    PRODUCT_NAME "Kick Clone"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Drum
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(KickClone PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/Dsp/Fft.h
    Source/Dsp/Events.h
    Source/Dsp/Isolate.h
    Source/Dsp/KickModel.h
    Source/Dsp/KickAnalyzer.h
    Source/Dsp/KickEngine.h)

target_include_directories(KickClone PRIVATE Source)

target_compile_definitions(KickClone PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# Opciones solo para compilar en Linux sin todas las librerias de escritorio (verificacion local / CI)
if(KICK_MINIMAL_LINUX)
    target_compile_definitions(KickClone PUBLIC
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XSHM=0 JUCE_USE_XRENDER=0)
endif()

target_link_libraries(KickClone
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(KICK_BUILD_TESTS)
    enable_testing()
    add_executable(dsp_test tests/dsp_test.cpp)
    target_include_directories(dsp_test PRIVATE Source/Dsp)
    add_test(NAME dsp_test COMMAND dsp_test)

    # Prueba de humo del procesador y la interfaz (necesita JUCE; en Linux, un servidor X o xvfb)
    juce_add_console_app(KickCloneSmokeTest PRODUCT_NAME "KickCloneSmokeTest")
    target_sources(KickCloneSmokeTest PRIVATE
        tests/ProcessorSmokeTest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(KickCloneSmokeTest PRIVATE Source)
    target_compile_definitions(KickCloneSmokeTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(KickCloneSmokeTest
        PRIVATE juce::juce_audio_processors juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags)
endif()
