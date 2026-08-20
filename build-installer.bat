@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "SKIP_TESTS="
if /I "%~1"=="/s" set "SKIP_TESTS=-SkipTests"
if /I "%~1"=="--silent" set "SKIP_TESTS=-SkipTests"
if "%SILENT%"=="1" set "SKIP_TESTS=-SkipTests"

echo [WimForge] Bootstrapping the Windows x64 toolchain and building the unsigned installer and portable archive.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap-build.ps1" -RepositoryPath "%ROOT%" %SKIP_TESTS%
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" echo [WimForge] Installer build failed with exit code %EXIT_CODE%.
exit /b %EXIT_CODE%
