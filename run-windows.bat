@echo off
:: SHIKGOO — Server starten (Windows)
cd /d "%~dp0"

if not exist web_server.exe (
    echo [FEHLER] web_server.exe nicht gefunden.
    echo Erst bauen: build-windows.bat
    pause
    exit /b 1
)

echo SHIKGOO startet...
echo.
echo   Kunden:  http://localhost:8080/customer
echo   Kellner: http://localhost:8080/waiter
echo.
echo Strg+C zum Beenden
echo.

web_server.exe
