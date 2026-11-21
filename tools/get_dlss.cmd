@echo off
rem Runs get_dlss.ps1 (beside this file): fetches NVIDIA's DLSS files and puts them beside nier.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get_dlss.ps1" %*
echo.
pause
