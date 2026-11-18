# Standalone input smoke test: no game, generated code, shader tools or graphics device.
cmake_minimum_required(VERSION 3.25)
project(gtav_input_tests LANGUAGES CXX)

if(WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "The smoke test uses the Linux/macOS SDL desktop window path")
endif()
find_package(rexglue 0.10.0 REQUIRED CONFIG)

set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../../gtav/src/native)
add_executable(gtav_input_tests input_test.cpp ${NATIVE}/input.cpp ${NATIVE}/menu.cpp ${NATIVE}/touch_controls.cpp)
target_compile_features(gtav_input_tests PRIVATE cxx_std_23)
target_include_directories(gtav_input_tests PRIVATE ${NATIVE})
target_link_libraries(gtav_input_tests PRIVATE rex::runtime)
rexglue_apply_target_settings(gtav_input_tests)

enable_testing()
add_test(NAME gameplay_input COMMAND gtav_input_tests)
add_test(NAME input_option_precedence COMMAND gtav_input_tests --check-opt-out)
