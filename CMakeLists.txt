cmake_minimum_required(VERSION 3.20)
project(joystick_penguin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JP_ENABLE_TTS "Announce mode changes using espeak-ng" ON)

include(FetchContent)
FetchContent_Declare(yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/releases/download/yaml-cpp-0.9.0/yaml-cpp-yaml-cpp-0.9.0.tar.gz
    URL_HASH SHA256=298593d9c440fd9034b8b193d96318b76d49bc97c6ceadb7b0836edf0b6d7539
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(YAML_CPP_BUILD_TOOLS OFF)
FetchContent_MakeAvailable(yaml-cpp)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
find_package(CLI11 REQUIRED)
if(JP_ENABLE_TTS)
    pkg_check_modules(ESPEAK_NG REQUIRED IMPORTED_TARGET espeak-ng)
endif()

set(JP_CORE_SOURCES src/config.cpp src/gesture_engine.cpp
    src/hardware.cpp src/joystick_preset.cpp src/output_frames.cpp)
if(JP_ENABLE_TTS)
    list(APPEND JP_CORE_SOURCES src/speech.cpp)
endif()
add_library(joystick_penguin_core ${JP_CORE_SOURCES})
target_include_directories(joystick_penguin_core PUBLIC include)
target_link_libraries(joystick_penguin_core PRIVATE yaml-cpp::yaml-cpp PkgConfig::LIBEVDEV)
if(JP_ENABLE_TTS)
    target_link_libraries(joystick_penguin_core PRIVATE PkgConfig::ESPEAK_NG)
    target_compile_definitions(joystick_penguin_core PUBLIC JOYSTICK_PENGUIN_ENABLE_TTS=1)
endif()
target_compile_options(joystick_penguin_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(joystick-penguin src/main.cpp)
target_link_libraries(joystick-penguin PRIVATE joystick_penguin_core CLI11::CLI11)

include(CTest)
if(BUILD_TESTING)
    add_executable(joystick_penguin_tests tests/config_test.cpp)
    target_link_libraries(joystick_penguin_tests PRIVATE joystick_penguin_core)
    target_compile_definitions(joystick_penguin_tests PRIVATE
        EXAMPLE_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/basic.yaml"
        GESTURES_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/gestures.yaml"
        CONTROLS_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/controls.yaml")
    add_test(NAME config_and_interfaces COMMAND joystick_penguin_tests)
    add_executable(gesture_engine_tests tests/gesture_engine_test.cpp)
    target_link_libraries(gesture_engine_tests PRIVATE joystick_penguin_core)
    add_test(NAME gesture_engine COMMAND gesture_engine_tests)
    add_executable(control_coverage_tests tests/control_coverage_test.cpp)
    target_link_libraries(control_coverage_tests PRIVATE joystick_penguin_core)
    add_test(NAME control_coverage COMMAND control_coverage_tests)
    add_executable(output_frames_tests tests/output_frames_test.cpp)
    target_link_libraries(output_frames_tests PRIVATE joystick_penguin_core)
    add_test(NAME output_frames COMMAND output_frames_tests)
    add_executable(modes_tests tests/modes_test.cpp)
    target_link_libraries(modes_tests PRIVATE joystick_penguin_core)
    add_test(NAME modes_and_tap_hold COMMAND modes_tests)
    add_executable(hardware_path_tests tests/hardware_path_test.cpp)
    target_link_libraries(hardware_path_tests PRIVATE joystick_penguin_core PkgConfig::LIBEVDEV)
    add_test(NAME hardware_path COMMAND hardware_path_tests)
    set_tests_properties(hardware_path PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()
