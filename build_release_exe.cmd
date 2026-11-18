@echo off
setlocal
rem Build the release exe (no game code inside; the launcher builds the game DLL locally).
rem Run from a VS x64 developer prompt with LLVM on PATH. Usage: build_release_exe.cmd [jobs]
set "JOBS=%~1"
if not defined JOBS set "JOBS=4"
if not defined GTAV_SDK_TREE (
  if exist "%~dp0rexglue-sdk\CMakeLists.txt" (
    set "GTAV_SDK_TREE=%~dp0rexglue-sdk"
  ) else (
    set "GTAV_SDK_TREE=E:\gtav-rex\rexglue-sdk"
  )
)
set "SDK=%GTAV_SDK_TREE%\out\install\win-amd64"
where clang++ >nul 2>nul || (echo Run from a VS x64 developer prompt with LLVM on PATH & exit /b 1)
if not exist "%SDK%\bin\SDL3rd.dll" (
  echo Install the patched RelWithDebInfo SDK in "%SDK%" first.
  exit /b 2
)
cmake -S "%~dp0gtav" --preset win-amd64-relwithdebinfo -B "%~dp0build-dll" -DGTAV_GAME_DLL=ON "-Drexglue_DIR=%SDK%/lib/cmake/rexglue" "-DREX_SDK_SRC=%GTAV_SDK_TREE%/src" || (echo CONFIGURE_FAILED & exit /b 4)
cmake --build "%~dp0build-dll" -j %JOBS% || (echo BUILD_FAILED & exit /b 5)
echo Built "%~dp0build-dll\gtav_fast.exe"
