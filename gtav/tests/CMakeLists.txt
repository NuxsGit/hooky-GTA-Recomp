cmake_minimum_required(VERSION 3.25)
project(gtav_graphics_tests LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)

add_executable(gtav_input_pacing_tests input_pacing_tests.cpp)
target_include_directories(gtav_input_pacing_tests PRIVATE ../src/native)
target_compile_features(gtav_input_pacing_tests PRIVATE cxx_std_20)
target_link_libraries(gtav_input_pacing_tests PRIVATE Threads::Threads)
add_test(NAME input_pacing_gate COMMAND gtav_input_pacing_tests)
set_tests_properties(input_pacing_gate PROPERTIES TIMEOUT 10)
add_executable(gtav_presentation_tests presentation_tests.cpp)
target_include_directories(gtav_presentation_tests PRIVATE ../src/native)
target_compile_features(gtav_presentation_tests PRIVATE cxx_std_20)
add_test(NAME presentation_policy COMMAND gtav_presentation_tests)

add_executable(gtav_graphics_policy_tests graphics_policy_tests.cpp)
target_include_directories(gtav_graphics_policy_tests PRIVATE ../src)
target_compile_features(gtav_graphics_policy_tests PRIVATE cxx_std_20)
add_test(NAME graphics_preset_policy COMMAND gtav_graphics_policy_tests)

add_executable(gtav_contact_rays_tests contact_rays_tests.cpp)
target_include_directories(gtav_contact_rays_tests PRIVATE ../src)
target_compile_features(gtav_contact_rays_tests PRIVATE cxx_std_20)
add_test(NAME contact_rays_policy COMMAND gtav_contact_rays_tests)

add_executable(gtav_upload_arena_tests upload_arena_tests.cpp)
target_include_directories(gtav_upload_arena_tests PRIVATE ../src)
target_compile_features(gtav_upload_arena_tests PRIVATE cxx_std_20)
add_test(NAME native_upload_arena COMMAND gtav_upload_arena_tests)

add_executable(gtav_fog_filter_tests fog_filter_tests.cpp)
target_include_directories(gtav_fog_filter_tests PRIVATE ../src)
target_compile_features(gtav_fog_filter_tests PRIVATE cxx_std_20)
add_test(NAME fog_strip_filter COMMAND gtav_fog_filter_tests)

add_executable(gtav_pipeline_cache_tests pipeline_cache_tests.cpp)
target_include_directories(gtav_pipeline_cache_tests PRIVATE ../src ../src/native)
target_compile_features(gtav_pipeline_cache_tests PRIVATE cxx_std_20)
add_test(NAME pipeline_cache_io COMMAND gtav_pipeline_cache_tests)

add_executable(gtav_resolve_tests resolve_tests.cpp)
target_include_directories(gtav_resolve_tests PRIVATE ../src/native)
target_compile_features(gtav_resolve_tests PRIVATE cxx_std_20)
add_test(NAME resolve_replay_policy COMMAND gtav_resolve_tests)

add_executable(gtav_pipeline_completion_tests pipeline_completion_tests.cpp)
target_include_directories(gtav_pipeline_completion_tests PRIVATE ../src/native)
target_compile_features(gtav_pipeline_completion_tests PRIVATE cxx_std_20)
add_test(NAME pipeline_completion COMMAND gtav_pipeline_completion_tests)
set_tests_properties(pipeline_completion PROPERTIES TIMEOUT 10)

add_executable(gtav_byteswap_simd_tests byteswap_simd_tests.cpp)
target_include_directories(gtav_byteswap_simd_tests PRIVATE ../src/native)
target_compile_features(gtav_byteswap_simd_tests PRIVATE cxx_std_20)
add_test(NAME byteswap_simd COMMAND gtav_byteswap_simd_tests)

add_executable(gtav_frame_hash_tests frame_hash_tests.cpp)
target_include_directories(gtav_frame_hash_tests PRIVATE ../src/native)
target_compile_features(gtav_frame_hash_tests PRIVATE cxx_std_20)
add_test(NAME frame_data_hash COMMAND gtav_frame_hash_tests)

if(NOT WIN32)
add_executable(gtav_safe_read_tests safe_read_tests.cpp)
target_include_directories(gtav_safe_read_tests PRIVATE ../src/native)
target_compile_features(gtav_safe_read_tests PRIVATE cxx_std_20)
add_test(NAME safe_read COMMAND gtav_safe_read_tests)
endif()

# Exercise the real decoder with synthetic physical memory and the pinned SDK's layout helpers.
set(GTAV_TEXTURE_SDK_SOURCE "" CACHE PATH "ReXGlue source tree (optional guest texture tests)")
set(GTAV_TEXTURE_PLUME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume" CACHE PATH "Plume source for texture format declarations")
if(GTAV_TEXTURE_SDK_SOURCE)
    set(texture_sdk "${GTAV_TEXTURE_SDK_SOURCE}/src/graphics/pipeline/texture")
    add_executable(gtav_guest_texture_tests guest_texture_tests.cpp ../src/native/textures.cpp
        "${texture_sdk}/util.cpp" "${texture_sdk}/info_formats.cpp" "${texture_sdk}/info.cpp" "${texture_sdk}/extent.cpp")
    if(WIN32)
        target_sources(gtav_guest_texture_tests PRIVATE "${GTAV_TEXTURE_SDK_SOURCE}/src/core/math_msvc.cpp")
    else()
        target_sources(gtav_guest_texture_tests PRIVATE "${GTAV_TEXTURE_SDK_SOURCE}/src/core/math_gcc.cpp")
    endif()
    target_include_directories(gtav_guest_texture_tests PRIVATE ../src/native "${GTAV_TEXTURE_PLUME_SOURCE}"
        "${GTAV_TEXTURE_SDK_SOURCE}/include" "${GTAV_TEXTURE_SDK_SOURCE}/thirdparty/xxHash"
        "${GTAV_TEXTURE_SDK_SOURCE}/thirdparty/fmt/include" "${GTAV_TEXTURE_SDK_SOURCE}/thirdparty/spdlog/include")
    target_compile_features(gtav_guest_texture_tests PRIVATE cxx_std_23)
    target_compile_definitions(gtav_guest_texture_tests PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL NOMINMAX WIN32_LEAN_AND_MEAN)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(gtav_guest_texture_tests PRIVATE PLUME_SDL3_VULKAN_ENABLED)
        target_include_directories(gtav_guest_texture_tests PRIVATE "${GTAV_TEXTURE_SDK_SOURCE}/thirdparty/sdl3/include")
    endif()
    add_test(NAME guest_texture_layout COMMAND gtav_guest_texture_tests)
endif()

# Optional offscreen shader test on Linux using the same patched Plume as the game.
# Set to DXC (or a wrapper that supplies DXC's Linux library search path).
set(GTAV_TEST_CONTACT_DXC "" CACHE FILEPATH "DXC executable for the optional Vulkan contact mask test")
if(GTAV_TEST_CONTACT_DXC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT GTAV_TEXTURE_SDK_SOURCE)
        message(FATAL_ERROR "Contact-mask Vulkan test requires Linux and GTAV_TEXTURE_SDK_SOURCE")
    endif()
    find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-shared)
    set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/contact_shaders)
    set(blit_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/../src/native/shaders/native_blit.hlsl)
    include(../cmake/native_contact_rays.cmake)
    gtav_generate_contact_rays("${shader_dir}" "${GTAV_TEST_CONTACT_DXC}")
    add_custom_command(OUTPUT "${shader_dir}/native_blit_vs.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${GTAV_TEST_CONTACT_DXC}" -T vs_6_0 -E VSMain -spirv -fspv-target-env=vulkan1.1
            -Vn g_native_blit_vs -Fh "${shader_dir}/native_blit_vs.h" "${blit_hlsl}"
        DEPENDS "${blit_hlsl}" VERBATIM)
    set(plume ${GTAV_TEXTURE_PLUME_SOURCE})
    add_executable(gtav_contact_rays_vulkan_tests contact_rays_vulkan_tests.cpp "${plume}/plume_vulkan.cpp"
        "${shader_dir}/native_contact_rays_ps.h" "${shader_dir}/native_blit_vs.h")
    target_include_directories(gtav_contact_rays_vulkan_tests PRIVATE ../src "${shader_dir}" "${plume}"
        "${plume}/contrib/volk" "${plume}/contrib/VulkanMemoryAllocator/include"
        "${GTAV_TEXTURE_SDK_SOURCE}/thirdparty/vulkan-headers/include")
    target_compile_features(gtav_contact_rays_vulkan_tests PRIVATE cxx_std_20)
    target_compile_definitions(gtav_contact_rays_vulkan_tests PRIVATE PLUME_SDL3_VULKAN_ENABLED)
    target_link_libraries(gtav_contact_rays_vulkan_tests PRIVATE SDL3::SDL3-shared Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME contact_rays_vulkan COMMAND gtav_contact_rays_vulkan_tests)
    set_tests_properties(contact_rays_vulkan PROPERTIES TIMEOUT 45)
endif()

set(GTAV_FMT_SOURCE "" CACHE PATH "ReXGlue's pinned thirdparty/fmt directory (optional text trace tests)")
if(GTAV_FMT_SOURCE)
    add_executable(gtav_frame_trace_tests frame_trace_tests.cpp)
    target_include_directories(gtav_frame_trace_tests PRIVATE ../src/native "${GTAV_FMT_SOURCE}/include")
    target_compile_features(gtav_frame_trace_tests PRIVATE cxx_std_20)
    target_compile_definitions(gtav_frame_trace_tests PRIVATE FMT_HEADER_ONLY)
    add_test(NAME frame_trace COMMAND gtav_frame_trace_tests)
endif()

add_library(gtav_test_preferences STATIC ../src/native/graphics_preferences.cpp)
target_include_directories(gtav_test_preferences PUBLIC ../src)
target_compile_features(gtav_test_preferences PUBLIC cxx_std_20)
add_executable(gtav_graphics_preferences_tests graphics_preferences_tests.cpp)
target_link_libraries(gtav_graphics_preferences_tests PRIVATE gtav_test_preferences)
add_test(NAME graphics_preferences COMMAND gtav_graphics_preferences_tests)

# Optional: exercise the real overlays without ReXGlue, SDL, a GPU or retail data, using the
# pinned ImGui sources from the same SDK checkout that builds the host.
set(GTAV_IMGUI_SOURCE "" CACHE PATH "ReXGlue's pinned thirdparty/imgui directory (optional UI tests)")
if(GTAV_IMGUI_SOURCE)
    include(../WorkshopCore.cmake)
    add_library(gtav_test_imgui STATIC ${GTAV_IMGUI_SOURCE}/imgui.cpp ${GTAV_IMGUI_SOURCE}/imgui_draw.cpp
        ${GTAV_IMGUI_SOURCE}/imgui_tables.cpp ${GTAV_IMGUI_SOURCE}/imgui_widgets.cpp)
    target_include_directories(gtav_test_imgui PUBLIC ${GTAV_IMGUI_SOURCE})
    target_compile_features(gtav_test_imgui PUBLIC cxx_std_20)
    target_compile_definitions(gtav_test_imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE
        IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS NOMINMAX WIN32_LEAN_AND_MEAN)
    add_executable(gtav_workshop_overlay_tests workshop_overlay_tests.cpp ${GTAV_WORKSHOP_CORE}
        ../src/sandbox/BuildMenuUI.cpp ../src/sandbox/world.cpp
        ../src/phone/PhoneShellUI.cpp ../src/phone/AdminCheatApp.cpp)
    target_include_directories(gtav_workshop_overlay_tests PRIVATE ../src)
    target_compile_definitions(gtav_workshop_overlay_tests PRIVATE GTAV_DESKTOP_WORKSHOP=1)
    target_link_libraries(gtav_workshop_overlay_tests PRIVATE gtav_test_imgui Threads::Threads)
    add_test(NAME workshop_overlay COMMAND gtav_workshop_overlay_tests)

    add_executable(gtav_graphics_menu_tests graphics_menu_tests.cpp ../src/native/graphics_menu_ui.cpp)
    target_link_libraries(gtav_graphics_menu_tests PRIVATE gtav_test_imgui gtav_test_preferences)
    add_test(NAME graphics_menu COMMAND gtav_graphics_menu_tests)
endif()

option(GTAV_TEST_PLAY_HUB "Test the real SDL hub with injected SDL events (requires GTAV_IMGUI_SOURCE)" OFF)
if(GTAV_TEST_PLAY_HUB)
    if(NOT GTAV_IMGUI_SOURCE)
        message(FATAL_ERROR "Set GTAV_IMGUI_SOURCE to the SDK's ImGui directory for imstb_truetype.h")
    endif()
    find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-shared)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(hub_dir ${CMAKE_CURRENT_SOURCE_DIR}/../src/frontend)
    set(hub_assets ${CMAKE_CURRENT_BINARY_DIR}/play_hub_assets.inc)
    file(GLOB art CONFIGURE_DEPENDS ${hub_dir}/assets/*.png ${hub_dir}/assets/*.ttf)
    add_custom_command(OUTPUT ${hub_assets}
        COMMAND ${Python3_EXECUTABLE} ${hub_dir}/embed_assets.py ${hub_assets}
        DEPENDS ${hub_dir}/embed_assets.py ${art} VERBATIM)
    add_executable(gtav_play_hub_tests play_hub_tests.cpp ${hub_assets})
    target_include_directories(gtav_play_hub_tests PRIVATE ${GTAV_IMGUI_SOURCE} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(gtav_play_hub_tests PRIVATE gtav_test_preferences SDL3::SDL3-shared)
    if(WIN32)
        add_custom_command(TARGET gtav_play_hub_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:gtav_play_hub_tests>)
    endif()
    add_test(NAME play_hub COMMAND gtav_play_hub_tests)
    set_tests_properties(play_hub PROPERTIES TIMEOUT 10)
endif()

if(MSVC)
    # The choices and fixtures include literal UTF-8 labels/paths.
    foreach(target gtav_graphics_preferences_tests gtav_graphics_policy_tests gtav_upload_arena_tests gtav_graphics_menu_tests gtav_play_hub_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
    endforeach()
endif()
