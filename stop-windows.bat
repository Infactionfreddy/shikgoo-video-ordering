@echo off
taskkill /F /IM web_server.exe >nul 2>&1
if %errorlevel%==0 (
    echo Server gestoppt.
) else (
    echo Server lief nicht.
)
