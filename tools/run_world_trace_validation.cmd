@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_split_validation.ps1" -Mode WorldTrace -KeepTestSettings
exit /b %ERRORLEVEL%
