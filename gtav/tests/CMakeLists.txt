cmake_minimum_required(VERSION 3.25)
project(gtav_graphics_tests LANGUAGES CXX)

enable_testing()
add_executable(gtav_presentation_tests presentation_tests.cpp)
target_include_directories(gtav_presentation_tests PRIVATE ../src/native)
target_compile_features(gtav_presentation_tests PRIVATE cxx_std_20)
add_test(NAME presentation_policy COMMAND gtav_presentation_tests)

add_executable(gtav_resolve_tests resolve_tests.cpp)
target_include_directories(gtav_resolve_tests PRIVATE ../src/native)
target_compile_features(gtav_resolve_tests PRIVATE cxx_std_20)
add_test(NAME resolve_replay_policy COMMAND gtav_resolve_tests)
