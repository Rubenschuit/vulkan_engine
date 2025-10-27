@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: windowsBuild.bat
:: Usage:
::   windowsBuild.bat [mode] [generator]
::     mode: debug | release (default) | test | clean
::     generator: mingw | msvc (default)
::
:: Examples:
::   windowsBuild.bat
::   windowsBuild.bat release msvc
::   windowsBuild.bat test mingw
::   windowsBuild.bat clean

set MODE=%~1
if "%MODE%"=="" set MODE=release
set GEN=%~2
if "%GEN%"=="" set GEN=msvc

:: Clean mode
if /I "%MODE%"=="clean" (
	GOTO :clean
)
GOTO :build

:clean
	if exist build rmdir /S /Q build
	if exist build_mingw rmdir /S /Q build_mingw
	if exist build_msvc rmdir /S /Q build_msvc
	for %%f in (shaders\*.spv) do if exist "%%~ff" del /Q /F "%%~ff"
	echo [INFO] Cleaned build directories and shaders
	exit /b 0

:build
:: Determine build type
set BUILD_TYPE=Debug
if /I "%MODE%"=="release" set BUILD_TYPE=Release

:: Extra CMake args
set EXTRA_CMAKE_ARGS=-DVE_BUILD_TESTS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if /I "%MODE%"=="test" set EXTRA_CMAKE_ARGS=-DVE_BUILD_TESTS=ON

:: Choose generator and build dir
if /I "%GEN%"=="msvc" (
	set CMAKE_GEN=Visual Studio 17 2022
	set BUILD_DIR=build
	set EXTRA_CMAKE_ARGS=%EXTRA_CMAKE_ARGS% -A x64
) else (
	set CMAKE_GEN=MinGW Makefiles
	set BUILD_DIR=build
)

echo [INFO] Configuring for %GEN% ("%CMAKE_GEN%"), %BUILD_TYPE%

:: Check if CMakeCache.txt exists (already configured)
if exist "%BUILD_DIR%\CMakeCache.txt" (
	echo [INFO] Build directory already configured, skipping configuration
) else (
	:: Configure
	set CONFIGURE_CMD=cmake -S . -B "%BUILD_DIR%" -G "%CMAKE_GEN%" %EXTRA_CMAKE_ARGS%
	if /I not "%GEN%"=="msvc" set CONFIGURE_CMD=%CONFIGURE_CMD% -DCMAKE_BUILD_TYPE=%BUILD_TYPE%

	echo [CMD ] %CONFIGURE_CMD%
	%CONFIGURE_CMD%
	if errorlevel 1 (
		echo [ERR ] CMake configure failed.
		exit /b %ERRORLEVEL%
	)
)

:: Build
if /I "%GEN%"=="msvc" (
	echo [INFO] Building (%BUILD_TYPE%)
	cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
) else (
	echo [INFO] Building (single-config: %BUILD_TYPE%)
	cmake --build "%BUILD_DIR%"
)
if errorlevel 1 (
	echo [ERR ] Build failed.
	exit /b %ERRORLEVEL%
)

:: Create symlink for IntelliSense if in Debug mode
if /I "%BUILD_TYPE%"=="Debug" (
	:: Create symlink from build/compile_commands.json to build/Debug/compile_commands.json
	if not exist "%BUILD_DIR%\compile_commands.json" (
		mklink "%BUILD_DIR%\compile_commands.json" "Debug\compile_commands.json" >nul 2>&1
	)
)

:: Tests
if /I "%MODE%"=="test" (
	echo [INFO] Running tests via CTest
	if /I "%GEN%"=="msvc" (
		ctest --test-dir "%BUILD_DIR%" -C %BUILD_TYPE% --output-on-failure
	) else (
		ctest --test-dir "%BUILD_DIR%" --output-on-failure
	)
		exit /b %ERRORLEVEL%
)

:: Run the app
set APP_PATH=%BUILD_DIR%\%BUILD_TYPE%\VeApp.exe

if exist "%APP_PATH%" (
	echo [INFO] Running %APP_PATH%
	"%APP_PATH%"
	exit /b %ERRORLEVEL%
) else (
	echo [ERR ] Executable not found: %APP_PATH%
	exit /b 2
)
