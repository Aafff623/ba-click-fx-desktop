@echo off
setlocal
echo Close ba-click-fx-desktop Host and Control Center first.
echo Run this file as administrator using the original Windows account.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\repair-stale-uninstall.ps1" -Apply
set "BAFX_REPAIR_EXIT=%ERRORLEVEL%"
if not "%BAFX_REPAIR_EXIT%"=="0" echo Repair failed. Keep the installation directory and the error above.
pause
exit /b %BAFX_REPAIR_EXIT%
