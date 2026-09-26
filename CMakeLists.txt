cmake_minimum_required(VERSION 3.20)
project(joystick_penguin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/releases/download/yaml-cpp-0.9.0/yaml-cpp-yaml-cpp-0.9.0.tar.gz
    URL_HASH SHA256=298593d9c440fd9034b8b193d96318b76d49bc97c6ceadb7b0836edf0b6d7539
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(YAML_CPP_BUILD_TOOLS OFF)
FetchContent_MakeAvailable(yaml-cpp)

add_library(joystick_penguin_core src/config.cpp)
target_include_directories(joystick_penguin_core PUBLIC include)
target_link_libraries(joystick_penguin_core PRIVATE yaml-cpp::yaml-cpp)
target_compile_options(joystick_penguin_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(joystick-penguin src/main.cpp)
target_link_libraries(joystick-penguin PRIVATE joystick_penguin_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(joystick_penguin_tests tests/config_test.cpp)
    target_link_libraries(joystick_penguin_tests PRIVATE joystick_penguin_core)
    target_compile_definitions(joystick_penguin_tests PRIVATE
        EXAMPLE_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/basic.yaml")
    add_test(NAME config_and_interfaces COMMAND joystick_penguin_tests)
endif()
