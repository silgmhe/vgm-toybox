@echo off
rem Double-click me to let Explorer open .vgm/.vgz/.s98/.dro/.gym with VGMToybox.
rem   RegisterFileTypes.cmd              just add it to the "Open with" list
rem   RegisterFileTypes.cmd /default     also make it the default for those formats
rem   RegisterFileTypes.cmd /unregister  undo it
rem Nothing here needs an administrator: everything is written under HKCU.
setlocal
set "ARGS="
if /I "%~1"=="/default"    set "ARGS=-SetDefault"
if /I "%~1"=="/unregister" set "ARGS=-Unregister"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0register-file-types.ps1" %ARGS%
echo.
pause
