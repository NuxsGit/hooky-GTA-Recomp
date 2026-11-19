# gtav launcher: first-run setup (the player's own discs + TU27 -> data\ and gtav_game.dll) and
# game launcher. Static C runtime, so it starts on a PC without the Visual C++ runtime and can
# tell the player to install it.
# macOS: GTAV.app with an AppKit setup window (src/main_mac.mm) that builds gtav_game.dylib.
# Linux: a setup window (GTK 3, dlopen'ed) or the terminal (src/main_linux.cpp) that builds
# gtav_game.so; static libstdc++ and libgcc, so it needs only glibc.
cmake_minimum_required(VERSION 3.25)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GTAV_VERSION LIMIT_COUNT 1)
if(NOT GTAV_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "loader/VERSION must be MAJOR.MINOR.PATCH")
endif()
project(gtav_launcher VERSION ${GTAV_VERSION} LANGUAGES CXX)

set(GTAV_STABLE_REPO "NuxsGit/hooky-GTA-Recomp" CACHE STRING "Trusted GitHub stable release repository (owner/name)")
set(GTAV_NIGHTLY_REPO "${GTAV_STABLE_REPO}" CACHE STRING "Trusted GitHub nightly release repository (owner/name)")
foreach(var IN ITEMS GTAV_STABLE_REPO GTAV_NIGHTLY_REPO)
    if(NOT "${${var}}" MATCHES "^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*$")
        message(FATAL_ERROR "${var} must be a GitHub owner/repository, not a URL")
    endif()
endforeach()
set(GTAV_NIGHTLY_BUILD "" CACHE STRING "Windows nightly build serial; empty for stable")
set(GTAV_BUILD_VERSION "${PROJECT_VERSION}")
if(NOT GTAV_NIGHTLY_BUILD STREQUAL "")
    string(LENGTH "${GTAV_NIGHTLY_BUILD}" _nightly_length)
    if(NOT WIN32 OR NOT GTAV_NIGHTLY_BUILD MATCHES "^[1-9][0-9]*$" OR _nightly_length GREATER 18)
        message(FATAL_ERROR "GTAV_NIGHTLY_BUILD requires Windows and a positive build serial (max 18 digits)")
    endif()
    string(APPEND GTAV_BUILD_VERSION "-nightly.${GTAV_NIGHTLY_BUILD}")
endif()
function(gtav_update_definitions target)
    target_compile_definitions(${target} PRIVATE GTAV_LAUNCHER_VERSION="${GTAV_BUILD_VERSION}"
        GTAV_STABLE_REPO="${GTAV_STABLE_REPO}" GTAV_NIGHTLY_REPO="${GTAV_NIGHTLY_REPO}")
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_executable(gtav_launcher WIN32
        src/main.cpp
        src/windows_update.cpp
        src/install.cpp
        src/sources.cpp
        src/launcher.manifest)  # visual styles, DPI awareness, long paths
    target_compile_definitions(gtav_launcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gtav_launcher PRIVATE comctl32 gdi32 ole32 shell32 uuid bcrypt winhttp dwmapi)
    if(MSVC)
        target_compile_options(gtav_launcher PRIVATE /utf-8)
    endif()
elseif(APPLE)
    # GTAV.app: the launcher is the bundle's executable; the package (gtav_fast, dylibs, rexglue,
    # tools/) goes to Contents/Resources/gtav (tools/release/make_release_macos.py).
    enable_language(OBJCXX)
    math(EXPR GTAV_MAC_BUILD_MAJOR "${PROJECT_VERSION_MAJOR} + 1")  # CFBundleVersion starts at 1
    add_executable(gtav_launcher MACOSX_BUNDLE
        src/main_mac.mm
        src/install.cpp
        src/sources.cpp)
    set_target_properties(gtav_launcher PROPERTIES
        OUTPUT_NAME GTAV
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${GTAV_MAC_BUILD_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
    target_compile_options(gtav_launcher PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(gtav_launcher PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
else()
    add_executable(gtav_launcher
        src/main_linux.cpp
        src/install.cpp
        src/sources.cpp)
    target_link_options(gtav_launcher PRIVATE -static-libstdc++ -static-libgcc)
    target_link_libraries(gtav_launcher PRIVATE ${CMAKE_DL_LIBS})
    # The setup window: GTK 3 headers only (types and enums); the library is dlopen'ed at run time,
    # so the launcher still starts without GTK (terminal setup).
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GTK3 QUIET gtk+-3.0)
    endif()
    if(GTK3_FOUND)
        target_include_directories(gtav_launcher SYSTEM PRIVATE ${GTK3_INCLUDE_DIRS})
        target_compile_definitions(gtav_launcher PRIVATE GTAV_GTK_UI)
    else()
        message(WARNING "gtk+-3.0 headers not found: the Linux launcher is built without its setup window")
    endif()
endif()

target_sources(gtav_launcher PRIVATE src/update.cpp src/update_http.cpp src/desktop_home.cpp)
gtav_update_definitions(gtav_launcher)
# Release staging checks this sidecar to catch packaging a launcher built for another version.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/launcher-build-version.in" CONTENT "${GTAV_BUILD_VERSION}\n" @ONLY)
add_custom_command(TARGET gtav_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/launcher-build-version.in"
            "${CMAKE_CURRENT_BINARY_DIR}/launcher-version.txt"
    VERBATIM)

# The phone builds (RunIphone, RunAndroid) translate shaders with the Adreno passes, C++ ports of
# tools/android/ubo_constants.py and lower_int64.py; the int64 lowering needs SPIRV-Tools (static,
# from the SDK's thirdparty tree).
target_sources(gtav_launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtav/src/native/adreno_shader_fix.cpp)
set(GTAV_SDK_THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-sdk/thirdparty CACHE PATH
    "rexglue-sdk/thirdparty (SPIRV-Tools for the Android shaders)")
set(_gtav_tp ${GTAV_SDK_THIRDPARTY})
if(EXISTS ${_gtav_tp}/spirv-tools/CMakeLists.txt)
    set(SPIRV-Headers_SOURCE_DIR ${_gtav_tp}/spirv-headers)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${_gtav_tp}/spirv-tools spirv-tools EXCLUDE_FROM_ALL)
    target_compile_definitions(gtav_launcher PRIVATE GTAV_HAVE_SPIRV_TOOLS=1)
    target_link_libraries(gtav_launcher PRIVATE SPIRV-Tools-static)
else()
    message(WARNING "rexglue-sdk/thirdparty/spirv-tools missing: the launcher cannot build for Android")
endif()

option(GTAV_LAUNCHER_TESTS "Build asset-free launcher tests" OFF)
if(GTAV_LAUNCHER_TESTS)
    enable_testing()
    add_executable(gtav_update_tests tests/update_tests.cpp src/update.cpp)
    target_include_directories(gtav_update_tests PRIVATE src)
    gtav_update_definitions(gtav_update_tests)
    add_test(NAME update_policy COMMAND gtav_update_tests)
    add_executable(gtav_runtime_policy_tests tests/runtime_policy_tests.cpp)
    target_include_directories(gtav_runtime_policy_tests PRIVATE ../gtav/src)
    add_test(NAME runtime_policy COMMAND gtav_runtime_policy_tests)
    add_executable(gtav_desktop_home_tests tests/desktop_home_tests.cpp src/desktop_home.cpp src/sources.cpp)
    target_include_directories(gtav_desktop_home_tests PRIVATE src ../gtav/src)
    if(WIN32)
        target_compile_definitions(gtav_desktop_home_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(gtav_desktop_home_tests PRIVATE shell32)
        if(MSVC)
            target_compile_options(gtav_desktop_home_tests PRIVATE /utf-8)
        endif()
    endif()
    add_test(NAME desktop_home COMMAND gtav_desktop_home_tests)
    find_package(Threads REQUIRED)
    set(_workshop ../gtav/src/sandbox)
    add_executable(gtav_sandbox_tests tests/sandbox_tests.cpp
        ${_workshop}/world.cpp ${_workshop}/protocol.cpp ${_workshop}/session.cpp
        ${_workshop}/courses.cpp ${_workshop}/paths.cpp ${_workshop}/scene.cpp)
    target_include_directories(gtav_sandbox_tests PRIVATE ${_workshop})
    target_link_libraries(gtav_sandbox_tests PRIVATE Threads::Threads)
    if(WIN32)
        target_compile_definitions(gtav_sandbox_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(gtav_sandbox_tests PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME sandbox_loopback COMMAND gtav_sandbox_tests)
    set_tests_properties(sandbox_loopback PROPERTIES TIMEOUT 25)
    if(WIN32)
        add_executable(gtav_windows_update_tests tests/windows_update_tests.cpp src/windows_update.cpp
            src/update.cpp src/update_http.cpp src/install.cpp src/sources.cpp ../gtav/src/native/adreno_shader_fix.cpp)
        target_include_directories(gtav_windows_update_tests PRIVATE src)
        target_compile_definitions(gtav_windows_update_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        gtav_update_definitions(gtav_windows_update_tests)
        target_link_libraries(gtav_windows_update_tests PRIVATE user32 bcrypt winhttp)
        add_test(NAME windows_update_transaction COMMAND gtav_windows_update_tests)

        # An older launcher for the exact-package test. Only the HTTP transport is replaced;
        # the dashboard timer, policy, installer, lock and restart helper are production code.
        add_executable(gtav_launcher_update_fixture WIN32 src/main.cpp src/windows_update.cpp
            src/update.cpp tests/update_transport_stub.cpp src/install.cpp src/sources.cpp src/desktop_home.cpp
            src/launcher.manifest ../gtav/src/native/adreno_shader_fix.cpp)
        target_include_directories(gtav_launcher_update_fixture PRIVATE src)
        target_compile_definitions(gtav_launcher_update_fixture PRIVATE
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN GTAV_LAUNCHER_VERSION="0.0.0")
        target_link_libraries(gtav_launcher_update_fixture PRIVATE comctl32 gdi32 ole32 shell32 uuid bcrypt dwmapi)
        if(MSVC)
            target_compile_options(gtav_launcher_update_fixture PRIVATE /utf-8)
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_library(gtav_test_vulkan SHARED tests/vulkan_stub.cpp)
        set_target_properties(gtav_test_vulkan PROPERTIES OUTPUT_NAME vulkan SOVERSION 1
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture-libs")
        add_test(NAME launcher_fixture COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/rebuild_test.py"
            $<TARGET_FILE:gtav_launcher> $<TARGET_FILE_DIR:gtav_test_vulkan>)
        set_tests_properties(launcher_fixture PROPERTIES TIMEOUT 90)
    endif()
endif()
