cmake_minimum_required(VERSION 3.25)
project(gtav_controller_check LANGUAGES C)

# Point CMAKE_PREFIX_PATH at the very same SDL/SDK install used by the package.
# This separate project doesn't need the SDK's game runtime or a video driver.
find_package(SDL3 3.2 REQUIRED CONFIG)
add_executable(gtav_controller_check controller_check.c)
target_compile_features(gtav_controller_check PRIVATE c_std_11)
if(WIN32)
    if(NOT TARGET SDL3::SDL3-shared)
        message(FATAL_ERROR "On Windows use the patched SDK's shared SDL3 build")
    endif()
    target_link_libraries(gtav_controller_check PRIVATE SDL3::SDL3-shared)
    add_custom_command(TARGET gtav_controller_check POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:gtav_controller_check> VERBATIM)
else()
    target_link_libraries(gtav_controller_check PRIVATE SDL3::SDL3)
endif()

enable_testing()
add_test(NAME controller_check_virtual COMMAND gtav_controller_check --self-test)
