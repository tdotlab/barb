@echo off
rem barb for Windows: build and test with MSVC (Visual Studio or its Build Tools).
rem   build.cmd             build build\barb.exe
rem   build.cmd test        the unit tests and the agent scenarios (under AddressSanitizer), then
rem                         the end-to-end test against a fake API (needs Python 3)
rem   build.cmd one NAME    build only build\NAME.exe (barb, barb-san, test_core, test_api,
rem                         test_agent, test_edit, test_keys, test_md, test_screen), as tools\mutate.py does
rem   build.cmd clean
rem Run it from any prompt: if cl.exe is not on the path, it finds Visual Studio with vswhere.
setlocal enableextensions
cd /d "%~dp0"

if /i "%~1"=="clean" ( if exist build rmdir /s /q build & exit /b 0 )

where cl.exe >nul 2>nul
if errorlevel 1 call :find_msvc || exit /b 1

if not exist build\san mkdir build\san
set CORE=src\core\arena.c src\core\str.c src\core\json.c src\core\api.c src\core\agent.c src\core\tools.c
set WIN=src\win32\ops.c src\ui\edit.c src\ui\md.c src\ui\screen.c src\win32\main.c
set LIBS=advapi32.lib
rem /utf-8: the sources are UTF-8. /std:c11: the core is ISO C11.
set WARN=/nologo /std:c11 /utf-8 /W4 /WX /wd4996
set SAN=/Zi /Od /fsanitize=address /MD /Fobuild\san\ /Fdbuild\san\

if /i not "%~1"=="one" goto :all
call :%~2
exit /b %errorlevel%

:all
call :barb || exit /b 1
if /i not "%~1"=="test" exit /b 0
call :test_core || exit /b 1
call :test_api || exit /b 1
call :test_agent || exit /b 1
call :test_edit || exit /b 1
call :test_keys || exit /b 1
call :test_md || exit /b 1
call :test_screen || exit /b 1
call :barb-san || exit /b 1
build\test_core.exe || exit /b 1
build\test_api.exe || exit /b 1
build\test_agent.exe || exit /b 1
build\test_edit.exe || exit /b 1
build\test_keys.exe || exit /b 1
build\test_md.exe || exit /b 1
build\test_screen.exe || exit /b 1
python tests\e2e.py build\barb-san.exe || exit /b 1
exit /b 0

:barb
cl %WARN% /O2 /Fobuild\ /Fe:build\barb.exe %CORE% %WIN% /link %LIBS%
exit /b %errorlevel%

:test_core
cl %WARN% %SAN% /Fe:build\test_core.exe tests\test_core.c src\core\arena.c src\core\str.c src\core\json.c || exit /b 1
goto :asan_dll

:test_api
cl %WARN% %SAN% /Fe:build\test_api.exe tests\test_api.c src\core\arena.c src\core\str.c src\core\json.c src\core\api.c || exit /b 1
goto :asan_dll

:test_agent
cl %WARN% %SAN% /Fe:build\test_agent.exe tests\test_agent.c %CORE% || exit /b 1
goto :asan_dll

:test_edit
cl %WARN% %SAN% /Fe:build\test_edit.exe tests\test_edit.c src\ui\edit.c || exit /b 1
goto :asan_dll

:test_keys
cl %WARN% %SAN% /Fe:build\test_keys.exe tests\test_keys.c src\ui\keys.c || exit /b 1
goto :asan_dll

:test_md
cl %WARN% %SAN% /Fe:build\test_md.exe tests\test_md.c src\ui\md.c src\ui\edit.c src\core\str.c src\core\arena.c || exit /b 1
goto :asan_dll

:test_screen
cl %WARN% %SAN% /Fe:build\test_screen.exe tests\test_screen.c src\ui\screen.c src\ui\md.c src\ui\edit.c src\core\str.c src\core\arena.c || exit /b 1
goto :asan_dll

:barb-san
cl %WARN% %SAN% /Fe:build\barb-san.exe %CORE% %WIN% /link %LIBS% || exit /b 1
goto :asan_dll

rem The sanitizer's runtime has to sit beside the programs that use it.
:asan_dll
for %%d in (clang_rt.asan_dynamic-x86_64.dll) do if not "%%~$PATH:d"=="" copy /y "%%~$PATH:d" build\ >nul
exit /b 0

:find_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo build.cmd: no cl.exe on the path and no Visual Studio installer found. Install the Visual Studio Build Tools with the C++ workload. & exit /b 1 )
set VSDIR=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo build.cmd: Visual Studio has no C++ tools. Add the "Desktop development with C++" workload. & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
