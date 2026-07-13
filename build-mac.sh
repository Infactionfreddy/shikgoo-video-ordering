#!/bin/bash
set -e

echo "=== SHIKGOO Build Script (macOS) ==="

# Check for compiler
if ! command -v cc &>/dev/null; then
    echo "Kein C-Compiler gefunden. Installiere Xcode Command Line Tools..."
    xcode-select --install
    echo "Nach der Installation erneut ausführen."
    exit 1
fi

echo "Compiler: $(cc --version | head -1)"

# OpenSSL prüfen
if ! command -v openssl &>/dev/null && ! [ -d /opt/homebrew/opt/openssl ] && ! [ -d /usr/local/opt/openssl ]; then
    echo "OpenSSL nicht gefunden. Installiere via Homebrew..."
    if ! command -v brew &>/dev/null; then
        echo "[FEHLER] Homebrew nicht gefunden. Bitte Homebrew installieren: https://brew.sh"
        exit 1
    fi
    brew install openssl
fi

# Build
echo "Kompiliere..."
make clean 2>/dev/null || true
make

echo ""
echo "Fertig! Starten mit:"
echo "  ./web_server"
echo "Webinterface: http://localhost:8080"
