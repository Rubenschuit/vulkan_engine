@echo off
setlocal EnableExtensions

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
	:: Remove everything in build directory except _deps
	if exist build (
		echo [INFO] Cleaning build directory (preserving _deps)
		cd build
		:: Remove all directories except _deps
		for /d %%d in (*) do (
			if not "%%d"=="_deps" (
				echo [INFO] Removing %%d
				rmdir /S /Q "%%d"
			)
		)
		:: Remove all files
		for %%f in (*.*) do (
			echo [INFO] Removing %%f
			del /Q /F "%%f"
		)
		:: Clean up FetchContent build directories but keep source
		if exist "_deps" (
			cd _deps
			for /d %%d in (*-build,*-subbuild) do (
				echo [INFO] Removing %%d
				rmdir /S /Q "%%d"
			)
			cd ..
		)
		cd ..
		echo [INFO] Cleaned build directory (preserved _deps source)
	)
	:: Clean other build directories
	if exist build_mingw rmdir /S /Q build_mingw
	if exist build_msvc rmdir /S /Q build_msvc

	:: Remove compiled shader files
	for %%f in (shaders\*.spv) do if exist "%%~ff" del /Q /F "%%~ff"
	echo [INFO] Cleaned build directories and shaders/*.spv
	exit /b 0
)

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

:: Configure
set CONFIGURE_CMD=cmake -S . -B "%BUILD_DIR%" -G "%CMAKE_GEN%" %EXTRA_CMAKE_ARGS%
if /I not "%GEN%"=="msvc" set CONFIGURE_CMD=%CONFIGURE_CMD% -DCMAKE_BUILD_TYPE=%BUILD_TYPE%

echo [CMD ] %CONFIGURE_CMD%
%CONFIGURE_CMD%
if errorlevel 1 (
	echo [ERR ] CMake configure failed.
	exit /b %ERRORLEVEL%
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
set APP_PATH=%BUILD_DIR%\VeApp.exe
if /I "%GEN%"=="msvc" set APP_PATH=%BUILD_DIR%\VeApp.exe

if exist "%APP_PATH%" (
	echo [INFO] Running %APP_PATH%
	"%APP_PATH%"
	exit /b %ERRORLEVEL%
) else (
	echo [ERR ] Executable not found: %APP_PATH%
	exit /b 2
)
