@echo off
setlocal

for %%I in ("%~dp0.") do set "rootDir=%%~fI"

call "%rootDir%\win_cmake_create_vs_project.bat"
if errorlevel 1 exit /b %errorlevel%

call "%rootDir%\win_cmake_build_install.bat"
if errorlevel 1 exit /b %errorlevel%

cmake --install "%rootDir%\vsbuild" --config Release --prefix "%rootDir%\install"
exit /b %errorlevel%
