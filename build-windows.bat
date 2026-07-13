@echo off
setlocal enabledelayedexpansion

echo === SHIKGOO Build Script (Windows / MinGW-w64) ===
echo.

:: --- Compiler suchen ---
set GCC=
set OPENSSL_INC=
set OPENSSL_LIB=

:: 1. MSYS2 MinGW-w64 (Standard-Installationspfad C:\)
if exist "C:\msys64\mingw64\bin\gcc.exe" (
    set GCC=C:\msys64\mingw64\bin\gcc.exe
    set GCC_BIN=C:\msys64\mingw64\bin
    set OPENSSL_INC=-IC:\msys64\mingw64\include
    set OPENSSL_LIB=-LC:\msys64\mingw64\lib
    goto :found
)

:: 2. MSYS2 MinGW-w64 auf D:\
if exist "D:\msys64\mingw64\bin\gcc.exe" (
    set GCC=D:\msys64\mingw64\bin\gcc.exe
    set GCC_BIN=D:\msys64\mingw64\bin
    set OPENSSL_INC=-ID:\msys64\mingw64\include
    set OPENSSL_LIB=-LD:\msys64\mingw64\lib
    goto :found
)

:: 3. WinLibs / standalone MinGW-w64
if exist "C:\mingw64\bin\gcc.exe" (
    set GCC=C:\mingw64\bin\gcc.exe
    set GCC_BIN=C:\mingw64\bin
    goto :found
)

:: 4. gcc im PATH
where gcc >nul 2>&1
if %errorlevel% equ 0 (
    set GCC=gcc
    goto :found
)

:: --- Nicht gefunden ---
echo [FEHLER] Kein GCC-Compiler gefunden.
echo.
echo Bitte MSYS2 installieren (empfohlen):
echo   1. https://www.msys2.org/ oeffnen und Installer herunterladen
echo   2. MSYS2 installieren (Standard: C:\msys64)
echo   3. MSYS2 MinGW x64 Terminal oeffnen
echo   4. Ausfuehren:  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl
echo   5. Dieses Script erneut starten
echo.
pause
exit /b 1

:found
if defined GCC_BIN set "PATH=%GCC_BIN%;%PATH%"
echo Compiler: %GCC%
"%GCC%" --version | findstr /i "gcc"
echo.

:: --- Build-Ordner anlegen ---
if not exist build mkdir build

:: --- Compile-Flags als Variable (kein ^ im for-Block noetig) ---
:: -I include\compat  stellt POSIX-Socket-Header fuer MinGW bereit
:: -Dclose=_compat_close  leitet close() auf Wrapper (WinSock vs CRT-Fd)
set "CF=-Wall -O2 -I include\compat -Iinclude %OPENSSL_INC% -DMSG_NOSIGNAL=0 -D_WIN32_WINNT=0x0601 -Dclose=_compat_close -Wno-deprecated-declarations -Wno-pointer-sign"

:: --- Quelldateien kompilieren ---
echo Kompiliere...
set OBJS=
set FAILED=0

for %%f in (src\*.c) do (
    set SRC=%%f
    set OBJ=build\%%~nf.o
    "%GCC%" !CF! -c "!SRC!" -o "!OBJ!"
    if !errorlevel! neq 0 (
        echo [FEHLER] Kompilierung fehlgeschlagen: !SRC!
        set FAILED=1
    ) else (
        set OBJS=!OBJS! !OBJ!
    )
)

if %FAILED% neq 0 (
    echo.
    echo [FEHLER] Build abgebrochen.
    pause
    exit /b 1
)

:: --- Linken ---
echo Linke...
"%GCC%" %OBJS% -o web_server.exe %OPENSSL_LIB% -lssl -lcrypto -lpthread -lws2_32 -lm

if %errorlevel% neq 0 (
    echo.
    echo [FEHLER] Linken fehlgeschlagen.
    echo Hinweis: OpenSSL fuer Windows benoetigt:
    echo   pacman -S mingw-w64-x86_64-openssl  (in MSYS2 MinGW x64)
    pause
    exit /b 1
)

echo.
echo Fertig! web_server.exe wurde erstellt.
echo Objektdateien liegen in build\
echo.
echo Starten mit:
echo   web_server.exe
echo Webinterface: http://localhost:8080
echo.
pause
