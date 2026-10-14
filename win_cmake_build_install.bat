@echo off
setlocal

for %%I in ("%~dp0.") do set "rootDir=%%~fI"
set "buildDir=%rootDir%\vsbuild"

if not exist "%buildDir%\CMakeCache.txt" (
    echo Windows build directory is not configured. Run win_cmake_create_vs_project.bat first.
    exit /b 1
)

cmake --build "%buildDir%" --config Release --parallel
exit /b %errorlevel%
