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

add_executable(gtav_resolve_tests resolve_tests.cpp)
target_include_directories(gtav_resolve_tests PRIVATE ../src/native)
target_compile_features(gtav_resolve_tests PRIVATE cxx_std_20)
add_test(NAME resolve_replay_policy COMMAND gtav_resolve_tests)

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
    foreach(target gtav_graphics_preferences_tests gtav_graphics_menu_tests gtav_play_hub_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
    endforeach()
endif()
