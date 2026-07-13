#!/bin/bash
set -e

CERT_DIR="/opt/shikgoo/certs"
HOSTS_ENTRY="127.0.0.1 shikgoo.local"

echo "=== SHIKGOO Dev Setup (macOS) ==="
echo ""

# ── 1. mkcert installieren ────────────────────────────────────────────────────
echo "[1/5] mkcert..."
if ! command -v mkcert &>/dev/null; then
    if ! command -v brew &>/dev/null; then
        echo "Homebrew fehlt: https://brew.sh"; exit 1
    fi
    brew install mkcert nss
fi
mkcert -install
echo "      CA in Chrome / Safari / Firefox vertrauenswürdig."
echo ""

# ── 2. Zertifikat generieren ──────────────────────────────────────────────────
echo "[2/5] Zertifikat für shikgoo.local..."
sudo mkdir -p "$CERT_DIR"
mkcert \
    -cert-file "$CERT_DIR/cert.pem" \
    -key-file  "$CERT_DIR/key.pem" \
    shikgoo.local localhost 127.0.0.1
sudo chmod 600 "$CERT_DIR/key.pem"
sudo chmod 644 "$CERT_DIR/cert.pem"
sudo chown -R "$(whoami)" /opt/shikgoo
echo "      Zertifikat erstellt."
echo ""

# ── 3. /etc/hosts ─────────────────────────────────────────────────────────────
echo "[3/5] shikgoo.local in /etc/hosts..."
if grep -qF "shikgoo.local" /etc/hosts; then
    echo "      Bereits vorhanden."
else
    echo "$HOSTS_ENTRY" | sudo tee -a /etc/hosts > /dev/null
    sudo dscacheutil -flushcache
    sudo killall -HUP mDNSResponder 2>/dev/null || true
    echo "      127.0.0.1 shikgoo.local eingetragen."
fi
echo ""

# ── 4. Build ──────────────────────────────────────────────────────────────────
echo "[4/5] Kompiliere..."
make clean 2>/dev/null || true
make
echo "      Build erfolgreich."
echo ""

# ── 5. Server starten ─────────────────────────────────────────────────────────
echo "[5/5] Starte Server..."
pkill web_server 2>/dev/null || true
pkill -f "dns-sd.*shikgoo" 2>/dev/null || true
sleep 0.3
# Port 80 (HTTP→HTTPS redirect) requires root
nohup sudo ./web_server > /tmp/shikgoo.log 2>&1 &
# Register shikgoo.local via mDNS so macOS resolves it instantly instead of
# timing out (macOS ignores /etc/hosts for .local and uses mDNS first).
nohup dns-sd -P "SHIKGOO" _http._tcp local 8443 shikgoo.local 127.0.0.1 \
    > /tmp/shikgoo-mdns.log 2>&1 &
sleep 0.5

if curl -sk https://shikgoo.local:8443/api/menu | grep -q "name"; then
    echo "      Server läuft — API antwortet."
else
    echo "      Server gestartet (Log: tail -f /tmp/shikgoo.log)"
fi
echo ""

echo "════════════════════════════════════════"
echo ""
echo "  https://shikgoo.local:8443"
echo ""
echo "  WICHTIG: Browser komplett neu starten!"
echo "  (nicht nur Tab — ganzen Browser schließen"
echo "   und wieder öffnen, sonst kein grünes Schloss)"
echo ""
echo "  Server stoppen:  pkill web_server"
echo "  Server-Log:      tail -f /tmp/shikgoo.log"
echo "  Tests:           bash tests/test_api.sh && bash tests/test_tls.sh"
echo "  Rückgängig:      mkcert -uninstall"
echo "                   sudo sed -i '' '/shikgoo.local/d' /etc/hosts"
echo ""
echo "════════════════════════════════════════"
