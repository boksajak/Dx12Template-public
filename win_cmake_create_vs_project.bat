@echo off
setlocal

for %%I in ("%~dp0.") do set "rootDir=%%~fI"
set "buildDir=%rootDir%\vsbuild"

cmake -S "%rootDir%\src" -B "%buildDir%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%

exit /b 0
