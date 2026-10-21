@echo off
setlocal

for %%I in ("%~dp0.") do set "rootDir=%%~fI"

call "%rootDir%\win_cmake_create_vs_project.bat"
if errorlevel 1 exit /b %errorlevel%

call "%rootDir%\win_cmake_build.bat"
if errorlevel 1 exit /b %errorlevel%

"%rootDir%\vsbuild\Release\Dx12Template.exe"
exit /b %errorlevel%
