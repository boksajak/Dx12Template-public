@echo off
setlocal EnableExtensions

rem Run from any location; the project root is the folder containing this script.
pushd "%~dp0" >nul || exit /b 1

rem Optional first argument overrides the output ZIP filename.
set "ZIP_NAME=%~1"
if not defined ZIP_NAME set "ZIP_NAME=source.zip"
if /I not "%ZIP_NAME:~-4%"==".zip" set "ZIP_NAME=%ZIP_NAME%.zip"
set "ZIP_PATH=%CD%\%ZIP_NAME%"

set "STAGE_ROOT=%CD%\out"
set "STAGE=%STAGE_ROOT%\package_source_%RANDOM%_%RANDOM%"

rem Validate that this looks like our project.
if not exist "src\CMakeLists.txt" (
    echo ERROR: Missing src\CMakeLists.txt
    goto :fail
)

rem Delete any pre-existing output ZIP before staging so it can never be
rem copied into the staging tree (and so a stale ZIP is never left behind
rem if a later step fails).
if exist "%ZIP_PATH%" (
    del /F /Q "%ZIP_PATH%"
    if errorlevel 1 goto :fail
)

mkdir "%STAGE%" >nul 2>&1 || goto :fail

rem Only package the portable source: the src and cmake subfolders, the
rem readme, and the root bat files. sdk (3rd-party dependencies), vsbuild,
rem install, out and other local/generated state are deliberately excluded.
robocopy "%CD%\src" "%STAGE%\src" /E /XJ /R:3 /W:1 /NFL /NDL /NJH /NP /XD "%CD%\src\assets"
set "ROBOCOPY_RESULT=%ERRORLEVEL%"
if %ROBOCOPY_RESULT% GEQ 8 goto :robocopy_fail

robocopy "%CD%\cmake" "%STAGE%\cmake" /E /XJ /R:3 /W:1 /NFL /NDL /NJH /NP
set "ROBOCOPY_RESULT=%ERRORLEVEL%"
if %ROBOCOPY_RESULT% GEQ 8 goto :robocopy_fail

copy /Y "%CD%\README.md" "%STAGE%\README.md" >nul
if errorlevel 1 goto :fail

copy /Y "%CD%\LICENSE" "%STAGE%\LICENSE" >nul
if errorlevel 1 goto :fail

for %%F in ("%CD%\*.bat") do (
    copy /Y "%%~fF" "%STAGE%\%%~nxF" >nul
    if errorlevel 1 goto :fail
)

rem Compress the staging directory contents at project-root level.
rem ZipFile is used instead of Compress-Archive so hidden files are not skipped.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference = 'Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory($env:STAGE, $env:ZIP_PATH, [System.IO.Compression.CompressionLevel]::Optimal, $false)"
if errorlevel 1 (
    echo ERROR: ZIP creation failed.
    goto :fail
)

rmdir /S /Q "%STAGE%" >nul 2>&1
rmdir "%STAGE_ROOT%" >nul 2>&1
popd >nul

echo Created: "%ZIP_PATH%"
exit /b 0

:robocopy_fail
set "ERR=%ROBOCOPY_RESULT%"
echo ERROR: Failed to stage project sources. Robocopy exit code: %ROBOCOPY_RESULT%
goto :cleanup_fail

:fail
set "ERR=%ERRORLEVEL%"

:cleanup_fail
if exist "%STAGE%" rmdir /S /Q "%STAGE%" >nul 2>&1
rmdir "%STAGE_ROOT%" >nul 2>&1
popd >nul
if "%ERR%"=="0" set "ERR=1"
exit /b %ERR%
