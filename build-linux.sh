#!/bin/bash
set -e

echo "=== SHIKGOO Build Script (Linux) ==="

# Detect package manager and install dependencies if needed
install_deps() {
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq
        sudo apt-get install -y gcc make libssl-dev
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y gcc make openssl-devel
    elif command -v pacman &>/dev/null; then
        sudo pacman -Sy --noconfirm gcc make openssl
    elif command -v zypper &>/dev/null; then
        sudo zypper install -y gcc make libopenssl-devel
    else
        echo "Paketmanager nicht erkannt. Bitte gcc, make und libssl-dev manuell installieren."
        exit 1
    fi
}

if ! command -v gcc &>/dev/null || ! command -v make &>/dev/null; then
    echo "gcc/make nicht gefunden, installiere..."
    install_deps
fi

echo "Compiler: $(gcc --version | head -1)"

echo "Kompiliere..."
make clean 2>/dev/null || true
make

echo ""
echo "Fertig! Starten mit:"
echo "  ./web_server"
echo "Webinterface: http://localhost:8080"
