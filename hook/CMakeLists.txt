cmake_minimum_required(VERSION 3.24)

project(FH6RadioBridgeHook LANGUAGES CXX)

add_library(FH6VersionProxy SHARED
    src/dllmain.cpp
    src/fmod_memory_probe.cpp
    src/fmod_radio_injector.cpp
    src/fmod_radio_stub.cpp
    src/game_event_monitor.cpp
    src/game_state_probe.cpp
    src/local_bridge_client.cpp
    src/logger.cpp
    src/shared_audio_reader.cpp
    src/version_exports.def
    src/version_proxy.cpp
    src/xaudio_shared_player.cpp
)

add_executable(SharedAudioReadTest
    src/fmod_radio_stub.cpp
    src/shared_audio_read_test.cpp
    src/shared_audio_reader.cpp
)

foreach(target IN ITEMS FH6VersionProxy SharedAudioReadTest)
target_compile_features(${target} PRIVATE cxx_std_20)
target_compile_definitions(${target} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
endforeach()

target_link_libraries(FH6VersionProxy PRIVATE xaudio2 psapi winhttp)

set_target_properties(FH6VersionProxy PROPERTIES
    OUTPUT_NAME "version"
)
