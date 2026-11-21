@echo off
rem N100 FAST — recompilation rapide sur Intel N100 4c / 8Go (6W)
rem Usage: build-n100.cmd [3]  (jobs, defaut 3 sur N100 = 1 coeur libre pour Windows/Defender)
rem Place ce .cmd a cote de build.cmd et lance le. Pas besoin de clean entre 2 builds.

setlocal
set JOBS=%1
if "%JOBS%"=="" set JOBS=3
echo [N100] jobs=%JOBS%  (4c Gracemont sans HT — 3 = fluide, 4 = max mais lag souris)

rem --- 1. Outils ---
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set CC=clang
set CXX=clang++
rem LLVM deja dans BuildTools\VC\Tools\Llvm\x64\bin via vcvars; sinon:
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;%PATH%

rem --- 2. Dossiers (METS SUR SSD, PAS eMMC) ---
rem Change E: si ton SSD est C:
set SRC=E:\gtav-native\gtav
set BLD=E:\gtav-native\build-n100
set REX=E:\gtav-rex\rexglue-sdk\out\install\win-amd64
set REXSRC=E:/gtav-rex/rexglue-sdk/src

if not exist "%SRC%" (
  echo SRC introuvable: %SRC%  — edite build-n100.cmd ligne SRC=
  exit /b 2
)

rem --- 3. Defender exclusion (1 fois, demande admin) ---
rem A lancer UNE FOIS en admin: powershell Add-MpPreference -ExclusionPath "E:\gtav-native"
rem Sinon chaque .obj est scanne = +40% temps sur N100.

rem --- 4. Configure (une fois) — UNITY + ccache si dispo ---
where ccache >nul 2>&1
if %errorlevel%==0 (
  set CCACHE=ON
  echo [N100] ccache detecte — cache actif
) else (
  set CCACHE=OFF
  echo [N100] installe ccache pour x2-x5 sur recompil: choco install ccache -y  OU  scoop install ccache
)

if not exist "%BLD%\build.ninja" (
  echo [%date% %time%] configure N100 (Unity + RelWithDebInfo sans LTO)
  cmake --preset win-amd64-relwithdebinfo -B "%BLD%" ^
    -DREXSDK="%REX%" -DREX_SDK_SRC="%REXSRC%" ^
    -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=8 ^
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF ^
    || (echo CONFIGURE_FAILED & exit /b 4)
) else (
  echo [%date% %time%] configure skip (deja fait — incremental)
)

rem --- 5. Build ---
echo [%date% %time%] build -j%JOBS%
cmake --build "%BLD%" -j %JOBS% || (echo BUILD_FAILED & exit /b 5)

rem --- 6. Copie rapide ---
copy /y "%BLD%\gtav.exe" "%BLD%\gtav_fast.exe" >nul
echo [%date% %time%] N100_DONE — binaire: %BLD%\gtav_fast.exe
echo Astuce: touche 1 fichier .cpp puis relance build-n100.cmd => 30s-90s (unity) vs 4-6min clean
endlocal
