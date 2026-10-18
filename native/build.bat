@echo off
rem RizFalling - build the d3d11 proxy DLL (MinGW-w64)
rem Requirements: gcc/g++ in PATH, or set MINGW_BIN to the bin folder.
setlocal
set GCC=gcc
set GXX=g++
if defined MINGW_BIN (
  set GCC=%MINGW_BIN%\gcc.exe
  set GXX=%MINGW_BIN%\g++.exe
)
"%GCC%" -O2 -DMINIZ_NO_STDIO -c miniz\miniz.c -o m_miniz.o || exit /b 1
"%GCC%" -O2 -DMINIZ_NO_STDIO -c miniz\miniz_tdef.c -o m_miniz_tdef.o || exit /b 1
"%GCC%" -O2 -DMINIZ_NO_STDIO -c miniz\miniz_tinfl.c -o m_miniz_tinfl.o || exit /b 1
"%GCC%" -O2 -DMINIZ_NO_STDIO -c miniz\miniz_zip.c -o m_miniz_zip.o || exit /b 1
"%GXX%" -shared -O2 -static -static-libgcc -static-libstdc++ ^
  -o d3d11.dll dllmain.cpp overlay.cpp chart_loader.cpp ^
  m_miniz.o m_miniz_tdef.o m_miniz_tinfl.o m_miniz_zip.o ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_impl_dx11.cpp ^
  -lole32 -lgdi32 -luuid -lshell32 -ld3dcompiler -Wl,--enable-stdcall-fixup
if errorlevel 1 exit /b 1
echo BUILD OK
dir d3d11.dll
