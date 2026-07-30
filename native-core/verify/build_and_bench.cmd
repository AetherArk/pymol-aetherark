@echo off
setlocal enabledelayedexpansion
REM Build + run the Phase-1 microbenchmark (native parse_f32 vs sscanf).
REM Assumes build_and_verify.cmd has already generated verify\gen (cxx glue) and
REM built the release staticlib; if not, it regenerates them.

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 1 )
call "%VCVARS%" || exit /b 1

set "HERE=%~dp0"
pushd "%HERE%.." || exit /b 1
set "CRATE=%CD%"

cargo build --release || (popd & exit /b 1)

if not exist "verify\gen\src\lib.rs.h" (
  if not exist "verify\gen\src" mkdir "verify\gen\src"
  if not exist "verify\gen\rust" mkdir "verify\gen\rust"
  cxxbridge --header > "verify\gen\rust\cxx.h" || (popd & exit /b 1)
  cxxbridge src\lib.rs --header > "verify\gen\src\lib.rs.h" || (popd & exit /b 1)
  cxxbridge src\lib.rs > "verify\gen\lib.rs.cc" || (popd & exit /b 1)
)

for /f "delims=" %%L in ('cargo rustc --release --quiet -- --print native-static-libs 2^>^&1 ^| findstr /C:"native-static-libs:"') do set "NATLIBS_LINE=%%L"
set "NATLIBS=!NATLIBS_LINE:*native-static-libs: =!"
if "!NATLIBS!"=="" set "NATLIBS=kernel32.lib ntdll.lib userenv.lib ws2_32.lib dbghelp.lib /defaultlib:msvcrt"

echo === compile + link bench (MSVC /O2 /MD) ===
cl /nologo /std:c++17 /EHsc /O2 /MD /I "verify\gen" ^
   /Fe:"verify\bench.exe" /Fo:"verify\gen\\" ^
   "verify\bench.cpp" "verify\gen\lib.rs.cc" ^
   /link "%CRATE%\target\release\native_core.lib" !NATLIBS! || (popd & exit /b 1)

echo === run benchmark ===
"verify\bench.exe" %1 %2
set "RC=!errorlevel!"
popd
exit /b !RC!
