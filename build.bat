@echo off
rem Wrapper for build.ps1 - usage: build.bat [-Arch x64|x86] [-Config Release|Debug|...] [-Clean]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
