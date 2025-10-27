@echo off
setlocal

:: windowsBuild.bat
:: Usage: windowsBuild.bat [debug|release|test|clean]
::   debug: builds in Debug mode and runs the app
::   release (default): builds in Release mode and runs the app
::   test: builds in Debug mode with tests enabled, runs all tests via CTest
::   clean: removes the build directory and compiled shader files

set MODE=%~1
if "%MODE%"=="" set MODE=release

if /I "%MODE%"=="clean" (
	if exist build rmdir /S /Q build
	if exist shaders\*.spv del /Q shaders\*.spv
	echo Cleaned build directory and shaders
	exit /b 0
)

:: Determine build type
set BUILD_TYPE=Release
if /I "%MODE%"=="debug" set BUILD_TYPE=Debug
if /I "%MODE%"=="test" set BUILD_TYPE=Debug

:: CMake args
set CMAKE_ARGS=-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if /I "%MODE%"=="test" set CMAKE_ARGS=%CMAKE_ARGS% -DVE_BUILD_TESTS=ON

:: Build directory
set BUILD_DIR=build

:: Configure and build
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %CMAKE_ARGS%
if errorlevel 1 exit /b %ERRORLEVEL%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% -j
if errorlevel 1 exit /b %ERRORLEVEL%

:: Run tests
if /I "%MODE%"=="test" (
	echo Running tests...
	ctest --test-dir "%BUILD_DIR%" -C %BUILD_TYPE% --output-on-failure
	exit /b %ERRORLEVEL%
)

:: Run the app
set APP_PATH=%BUILD_DIR%\%BUILD_TYPE%\VeApp.exe
"%APP_PATH%"
