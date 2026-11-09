# Installation

[Zurück zur README](../README.md)

## Voraussetzungen

- Linux, macOS (oder Windows mit eigenem Build-Skript, hier nicht im Detail geprüft)
- `gcc`/`cc`, `make`, OpenSSL-Header (`libssl-dev`, `openssl-devel` bzw. Homebrew `openssl`)
- Optional: `avahi-daemon` (mDNS), `mkcert` (macOS-Entwicklung), `python3` plus `pip3 install websockets` für Tests
- Root-Rechte nur für Port 80 (HTTP-Weiterleitung) bzw. `/opt/shikgoo`

## Variante A: Raspberry Pi (empfohlen)

```bash
sudo bash install-pi.sh
```

Das Skript:

1. installiert `gcc make libssl-dev avahi-daemon`
2. baut den Server
3. kopiert `web_server`, `frontend/`, `menu.json` nach `/opt/shikgoo`
4. erzeugt ein selbstsigniertes Zertifikat (365 Tage, SAN `shikgoo.local` und aktuelle IP) in `/opt/shikgoo/certs`
5. setzt den Hostnamen `shikgoo` und aktiviert Avahi
6. legt den systemd-Service `shikgoo` an und startet ihn

Erreichbar unter `https://shikgoo.local:8443`. Auf iOS das Zertifikat über `https://<IP>:8443/cert.pem` laden und unter Einstellungen > Allgemein > Info > Zertifikat-Vertrauenseinstellungen aktivieren.

Nützlich: `sudo systemctl status|stop|restart shikgoo`, `sudo journalctl -u shikgoo -f`.

## Variante B: Manuell (Linux/macOS)

```bash
bash build-linux.sh     # oder build-mac.sh
sudo mkdir -p /opt/shikgoo/certs
# cert.pem und key.pem dort ablegen
./web_server
```

Der Server muss aus dem Projekt-Root laufen, da `menu.json`, `orders.json` und `frontend/` relativ geladen werden. Ohne `menu.json` bricht der Start ab.

## Variante C: macOS-Entwicklung

```bash
bash dev-setup-macos.sh
```

Installiert `mkcert`, erzeugt Zertifikate, trägt `shikgoo.local` in `/etc/hosts` ein, baut und startet den Server (mit `sudo`) und registriert den Namen per `dns-sd`. Browser danach komplett neu starten.

## Update

```bash
git pull
sudo bash install-pi.sh      # Pi: baut neu, behält orders.json und Zertifikat-Generierung läuft erneut
```

Manuell: `make clean && make`, Server neu starten. Hinweis: Das Pi-Skript erzeugt bei jedem Lauf ein neues Zertifikat; Clients müssen ihm dann erneut vertrauen.

## Stoppen

`stop-linux.sh`, `stop-mac.sh`, `stop-windows.bat` (beenden den Prozess `web_server`).

## Deinstallation

```bash
sudo systemctl disable --now shikgoo
sudo rm /etc/systemd/system/shikgoo.service
sudo systemctl daemon-reload
sudo rm -rf /opt/shikgoo
```

macOS-Entwicklung: `mkcert -uninstall` und den Eintrag `shikgoo.local` aus `/etc/hosts` entfernen.
