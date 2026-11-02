@echo off
setlocal

:: windowsBuild.bat
:: Usage: windowsBuild.bat [debug|release|test|tracy|clean] [vs2022|vs2026] [norun]
::   debug: builds in debug mode and runs the app
::   release (default): builds in release mode and runs the app
::   test: builds in debug mode with tests enabled, runs all tests via CTest
::   tracy: builds in RelWithDebInfo with Tracy profiler enabled and runs the app
::   clean: removes the build directory and compiled shader files
::   norun: build only, don't launch the app afterwards (it does not self-close,
::          so this is what you want for scripted or unattended builds)

set MODE=%~1
set VS_VERSION=%~2
set RUN_APP=1
for %%A in (%*) do (
	if /I "%%~A"=="norun" (
		set RUN_APP=0
		if /I "%MODE%"=="norun" set MODE=
		if /I "%VS_VERSION%"=="norun" set VS_VERSION=
	)
)
if "%MODE%"=="" set MODE=release
if "%VS_VERSION%"=="" set VS_VERSION=vs2026

if /I "%MODE%"=="clean" (
	if exist build rmdir /S /Q build
	if exist shaders\*.spv del /Q shaders\*.spv
	echo Cleaned build directory and shaders
	exit /b 0
)

:: Determine build type
set BUILD_TYPE=release
if /I "%MODE%"=="debug" set BUILD_TYPE=debug
if /I "%MODE%"=="test" set BUILD_TYPE=debug
if /I "%MODE%"=="tracy" set BUILD_TYPE=RelWithDebInfo

:: Set Visual Studio generator
if /I "%VS_VERSION%"=="vs2022" set VS_GENERATOR=Visual Studio 17 2022
if /I "%VS_VERSION%"=="vs2026" set VS_GENERATOR=Visual Studio 18 2026

echo Building in %BUILD_TYPE% mode
echo Using %VS_GENERATOR%
:: CMake args
set TESTS_FLAG=OFF
if /I "%MODE%"=="test" set TESTS_FLAG=ON
set TRACY_FLAG=OFF
if /I "%MODE%"=="tracy" set TRACY_FLAG=ON
set CMAKE_ARGS=-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVE_BUILD_TESTS=%TESTS_FLAG% -DVE_ENABLE_TRACY=%TRACY_FLAG%

:: Build directory
set BUILD_DIR=build

:: Configure and build
cmake -S . -B "%BUILD_DIR%" -G "%VS_GENERATOR%" -A x64 %CMAKE_ARGS%
if errorlevel 1 exit /b %ERRORLEVEL%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% -j
if errorlevel 1 exit /b %ERRORLEVEL%

:: Run tests
if /I not "%MODE%"=="test" goto run_app
echo Running tests...
ctest --test-dir "%BUILD_DIR%" -C %BUILD_TYPE% --output-on-failure
exit /b %ERRORLEVEL%

:: Run the app
:run_app
if "%RUN_APP%"=="0" (
	echo Build complete ^(norun^)
	exit /b 0
)
set APP_PATH=%BUILD_DIR%\%BUILD_TYPE%\VeApp.exe
"%APP_PATH%"
