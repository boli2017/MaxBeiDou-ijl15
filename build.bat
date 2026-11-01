@echo off
rem Convenience wrapper: double-click to build Release|Win32.
rem Pass-through example:  build.bat -Config Debug -Deploy
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
