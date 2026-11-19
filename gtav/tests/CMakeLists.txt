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

# Optional: exercise the real overlay without ReXGlue, SDL, a GPU or retail data, using the
# ImGui sources from the same SDK checkout that builds the host.
set(GTAV_IMGUI_SOURCE "" CACHE PATH "ReXGlue's pinned thirdparty/imgui directory (optional Workshop UI test)")
if(GTAV_IMGUI_SOURCE)
    include(../WorkshopCore.cmake)
    find_package(Threads REQUIRED)
    add_executable(gtav_workshop_overlay_tests workshop_overlay_tests.cpp ${GTAV_WORKSHOP_CORE}
        ../src/sandbox/BuildMenuUI.cpp ../src/sandbox/world.cpp
        ../src/phone/PhoneShellUI.cpp ../src/phone/AdminCheatApp.cpp
        ${GTAV_IMGUI_SOURCE}/imgui.cpp ${GTAV_IMGUI_SOURCE}/imgui_draw.cpp
        ${GTAV_IMGUI_SOURCE}/imgui_tables.cpp ${GTAV_IMGUI_SOURCE}/imgui_widgets.cpp)
    target_compile_features(gtav_workshop_overlay_tests PRIVATE cxx_std_20)
    target_include_directories(gtav_workshop_overlay_tests PRIVATE ../src ${GTAV_IMGUI_SOURCE})
    target_compile_definitions(gtav_workshop_overlay_tests PRIVATE GTAV_DESKTOP_WORKSHOP=1 IMGUI_ENABLE_TEST_ENGINE
        IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gtav_workshop_overlay_tests PRIVATE Threads::Threads)
    add_test(NAME workshop_overlay COMMAND gtav_workshop_overlay_tests)
endif()
