# Konfiguration

[Zurück zur README](../README.md)

Der Server liest **keine Umgebungsvariablen** und keine Kommandozeilenargumente. Einstellungen sind Compile-Time-Konstanten; nach einer Änderung ist `make` nötig.

## Konstanten

| Name | Datei | Standard | Bedeutung |
|---|---|---|---|
| `PORT` | `include/types.h` | 8443 | HTTPS-/WSS-Port |
| `HTTP_PORT` | `src/main.c` | 80 | Redirect-Port (braucht root, sonst Warnung und kein Redirect) |
| `CERT_PATH` | `src/main.c` | `/opt/shikgoo/certs/cert.pem` | Zertifikat |
| `KEY_PATH` | `src/main.c` | `/opt/shikgoo/certs/key.pem` | privater Schlüssel (Inhalt nie ins Repo!) |
| `FRONTEND_DIR` | `include/types.h` | `frontend` | Statische Dateien, relativ zum CWD |
| `MAX_ORDERS` | `include/types.h` | 1000 | Maximale offene Bestellungen |
| `MAX_MENU` | `include/types.h` | 100 | Maximale Menüeinträge |
| `MAX_WS` | `include/types.h` | 64 | Gleichzeitige WebSockets |
| `MAX_CALLS` | `include/types.h` | 200 | Kellnerrufe (inkl. erledigter) |
| `RECV_BUF` | `include/types.h` | 8192 | Maximale Request-/Body-Größe |

## Dateien

| Datei | Zweck |
|---|---|
| `menu.json` | Speisekarte, nur beim Start geladen (kein Hot-Reload) |
| `orders.json` | Offene Bestellungen, automatisch geschrieben |
| `calls.json` | Offene Kellnerrufe, automatisch geschrieben |

### Format `menu.json`

| Feld | Typ | Hinweis |
|---|---|---|
| `id` | int > 0 | eindeutig |
| `name` | string | max. 63 Zeichen |
| `price` | int | in Cent |
| `category` | string | max. 31 Zeichen |
| `description` | string | max. 127 Zeichen |
| `image_url` | string | wird eingelesen, API liefert immer leeren Wert |

## Build-Optionen (Makefile)

| Variable | Standard |
|---|---|
| `CC` | `cc` |
| `CFLAGS` | `-Wall -Wextra -O2 -Iinclude` plus OpenSSL-Flags |
| `LDFLAGS` | `-lpthread` plus OpenSSL-Libs |

OpenSSL wird per `pkg-config`, dann Homebrew-Pfaden, dann `-lssl -lcrypto` gefunden.

## WebSocket-URL-Parameter

| Parameter | Bedeutung |
|---|---|
| `role=waiter` | Verbindung als Kellner |
| `table=N` | Tisch 1-20 (sonst 0) |

## Hooks/Filter

Nicht vorhanden.
