@echo off
rem Pager Bridge one-click installer. Double-click to run.
rem Pass -InstallDir / -AutoStart / -SkipSTT through to install.ps1, e.g.:
rem     install.cmd -InstallDir D:\pager-app -AutoStart
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
