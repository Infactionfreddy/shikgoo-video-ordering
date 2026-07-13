#!/bin/bash
set -e

# Must run from project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="/opt/shikgoo"
SERVICE_NAME="shikgoo"
PORT=8443

echo "=== SHIKGOO Raspberry Pi Installer ==="
echo "Installiert nach: $INSTALL_DIR"
echo ""

# Root check
if [[ $EUID -ne 0 ]]; then
    echo "Dieses Script muss als root ausgefuehrt werden."
    echo "  sudo bash install-pi.sh"
    exit 1
fi

# Install build dependencies
echo "[1/5] Installiere Abhaengigkeiten..."
apt-get update -qq
apt-get install -y gcc make libssl-dev avahi-daemon

# Build
echo "[2/5] Kompiliere..."
cd "$SCRIPT_DIR"
make clean 2>/dev/null || true
make
echo "Build erfolgreich."

# Deploy files
echo "[3/5] Kopiere Dateien nach $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR/frontend"
cp web_server "$INSTALL_DIR/"
cp -r frontend/* "$INSTALL_DIR/frontend/"
cp menu.json "$INSTALL_DIR/"

# Copy orders.json only if it doesn't exist (preserve existing data)
if [[ ! -f "$INSTALL_DIR/orders.json" ]]; then
    cp orders.json "$INSTALL_DIR/" 2>/dev/null || echo '{"orders":[]}' > "$INSTALL_DIR/orders.json"
fi

chmod +x "$INSTALL_DIR/web_server"
echo "Dateien kopiert."

# Generate TLS certificate
echo "[3b/5] Generiere TLS-Zertifikat..."
CURRENT_IP=$(hostname -I | awk '{print $1}')
CERT_DIR="$INSTALL_DIR/certs"
mkdir -p "$CERT_DIR"
openssl req -x509 \
    -newkey rsa:2048 \
    -keyout "$CERT_DIR/key.pem" \
    -out    "$CERT_DIR/cert.pem" \
    -days   365 \
    -nodes \
    -subj   "/CN=shikgoo.local" \
    -addext "subjectAltName=DNS:shikgoo.local,IP:$CURRENT_IP"
chmod 600 "$CERT_DIR/key.pem"
chmod 644 "$CERT_DIR/cert.pem"
echo "Zertifikat generiert fuer DNS:shikgoo.local und IP:$CURRENT_IP"

# Set hostname and enable mDNS
echo "[3c/5] Setze Hostname fuer mDNS..."
hostnamectl set-hostname shikgoo
systemctl enable --now avahi-daemon
systemctl restart avahi-daemon
echo "Hostname gesetzt: shikgoo (erreichbar als shikgoo.local)"

# Create systemd service
echo "[4/5] Erstelle systemd Service..."
cat > "/etc/systemd/system/${SERVICE_NAME}.service" <<EOF
[Unit]
Description=SHIKGOO Restaurant Backend
After=network.target

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
ExecStart=$INSTALL_DIR/web_server
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# Enable and start service
echo "[5/5] Aktiviere und starte Service..."
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

# Get IP address
IP=$(hostname -I | awk '{print $1}')

echo ""
echo "=== Installation abgeschlossen ==="
echo ""
echo "Service Status:"
systemctl status "$SERVICE_NAME" --no-pager -l
echo ""
echo "Webinterface erreichbar unter:"
echo "  https://shikgoo.local:8443  (nach Zertifikat-Vertrauen im Browser)"
echo "  https://$IP:8443"
echo ""
echo "WICHTIG: Beim ersten Aufruf im Browser das selbst-signierte Zertifikat akzeptieren."
echo "iOS Safari: Zertifikat herunterladen unter https://$IP:8443/cert.pem"
echo "  -> Einstellungen -> Allgemein -> Info -> Zertifikat-Vertrauenseinstellungen"
echo ""
echo "Nuetzliche Befehle:"
echo "  sudo systemctl status $SERVICE_NAME   # Status anzeigen"
echo "  sudo systemctl stop $SERVICE_NAME     # Stoppen"
echo "  sudo systemctl restart $SERVICE_NAME  # Neu starten"
echo "  sudo journalctl -u $SERVICE_NAME -f   # Logs live verfolgen"
