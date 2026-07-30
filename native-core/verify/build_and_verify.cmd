@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM Phase-1 seam verification: build the Rust crate, run its tests, generate the
REM cxx glue, then compile+link+run the differential driver with MSVC.
REM Proves the Rust<->C++ (windows-msvc) build seam end to end without cmake or
REM the full PyMOL build.
REM ===========================================================================

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" || exit /b 1

REM native-core dir is the parent of this script's dir.
set "HERE=%~dp0"
pushd "%HERE%.." || exit /b 1
set "CRATE=%CD%"

echo === cargo test (release) ===
cargo test --release || (popd & exit /b 1)

echo === cargo build (staticlib, release) ===
cargo build --release || (popd & exit /b 1)

echo === locate cxxbridge CLI ===
where cxxbridge >nul 2>nul
if errorlevel 1 (
  echo cxxbridge not found; installing pinned version...
  cargo install cxxbridge-cmd --version 1.0.130 || (popd & exit /b 1)
)

echo === generate cxx glue ===
REM cxxbridge's generated .cc self-includes the header using the INPUT path
REM ("src/lib.rs.h"), so lay the generated tree out under verify\gen with that
REM same prefix and put verify\gen on the include path.
if not exist "verify\gen\src" mkdir "verify\gen\src"
if not exist "verify\gen\rust" mkdir "verify\gen\rust"
cxxbridge --header > "verify\gen\rust\cxx.h" || (popd & exit /b 1)
cxxbridge src\lib.rs --header > "verify\gen\src\lib.rs.h" || (popd & exit /b 1)
cxxbridge src\lib.rs > "verify\gen\lib.rs.cc" || (popd & exit /b 1)

echo === discover native static libs required by the Rust staticlib ===
REM Rust prints the exact system libs the staticlib needs to satisfy at link time.
for /f "delims=" %%L in ('cargo rustc --release --quiet -- --print native-static-libs 2^>^&1 ^| findstr /C:"native-static-libs:"') do set "NATLIBS_LINE=%%L"
set "NATLIBS=!NATLIBS_LINE:*native-static-libs: =!"
if "!NATLIBS!"=="" set "NATLIBS=ws2_32.lib userenv.lib advapi32.lib bcrypt.lib ntdll.lib kernel32.lib"
echo native-static-libs: !NATLIBS!

echo === compile + link driver (MSVC) ===
REM /MD: match Rust's default dynamic CRT (the cxx glue in native_core.lib is
REM compiled /MD via the cc crate). cl's command-line default is /MT, which would
REM otherwise trip LNK2038 RuntimeLibrary mismatch.
cl /nologo /std:c++17 /EHsc /O2 /MD /I "verify\gen" ^
   /Fe:"verify\driver.exe" ^
   /Fo:"verify\gen\\" ^
   "verify\driver.cpp" "verify\gen\lib.rs.cc" ^
   /link "%CRATE%\target\release\native_core.lib" !NATLIBS! || (popd & exit /b 1)

echo === run differential driver ===
"verify\driver.exe"
set "RC=!errorlevel!"
popd
exit /b !RC!
