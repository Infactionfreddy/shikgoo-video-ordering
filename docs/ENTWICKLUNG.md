# Entwicklung

[Zurück zur README](../README.md)

## Lokale Umgebung

```bash
bash dev-setup-macos.sh    # macOS: mkcert, hosts, Build, Start
# oder manuell:
make
./web_server               # im Projekt-Root
```

Frontend-Änderungen wirken sofort (Dateien werden mit `no-cache` ausgeliefert); Backend-Änderungen erfordern `make` und Neustart. Zertifikate müssen unter `/opt/shikgoo/certs` liegen (Pfad ist hartkodiert).

## Code-Struktur

- `src/` und `include/`: Backend (siehe [Architektur](ARCHITEKTUR.md)).
- `frontend/`: `app.js` (gemeinsame Helfer, hängt alles an `window`), `webrtc.js`, `customer.js`, `waiter.js`. Skript-Reihenfolge: `app.js`, `webrtc.js`, rollenspezifisch.
- `include/compat/`: Winsock-Shims für Windows.

## Tests

Alle Tests sind Integrationsskripte und benötigen einen laufenden Server.

```bash
bash tests/test_api.sh
bash tests/test_tls.sh
pip3 install websockets
python3 tests/test_ws.py
python3 tests/test_ws_signaling.py
python3 tests/test_webrtc_signaling.py
```

Hinweis: `test_ws.py` nutzt `ws://localhost:8080`, `test_api.sh` und `test_tls.sh` nutzen `https://localhost:8443`. Die WS-Tests passen vermutlich nicht zum aktuellen TLS-only-Server und sind eventuell veraltet (Annahme; Signaling-Tests wurden nicht geprüft). Es gibt keine Unit-Tests und keine CI.

## Coding-Standards (aus dem Code abgeleitet)

- C mit `-Wall -Wextra`, POSIX; Fehler über negative `LOGIC_*`-Codes.
- Geschäftslogik und Locking nur in `logic.c`; `api.c` bleibt dünn.
- Persistenz und Broadcast erst nach dem Unlock; nie `g_lock` und `ws_lock` zusammen halten.
- Beträge in Cent; JSON-Aufbau mit cJSON (Aufrufer löscht mit `cJSON_Delete`).
- Reihenfolge von `STATUS_STR` muss dem `Status`-Enum entsprechen.
- Frontend: Vanilla-JS, Ausgaben vor `innerHTML` mit `escapeHtml` escapen.

## Neue Endpunkte hinzufügen

1. Logik in `logic.c`/`logic.h` ergänzen.
2. Handler in `api.c`/`api.h`.
3. Route in `handle_conn()` in `main.c`.
4. Test in `tests/test_api.sh` ergänzen.

## Release-Ablauf

Es ist kein formaler Release-Prozess im Repository definiert. Vorschlag (Annahme): Tests ausführen, `git tag vX.Y.Z`, auf dem Zielgerät `sudo bash install-pi.sh` ausführen.
