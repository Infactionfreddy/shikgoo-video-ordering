# API

[Zurück zur README](../README.md)

Basis: `https://<host>:8443`. Antworten sind JSON, CORS ist auf `*` gesetzt. Es gibt keine Authentifizierung. Preise in Cent. Fehler: `{"error":"..."}`.

## REST

| Methode | Pfad | Beschreibung | Antwort |
|---|---|---|---|
| GET | `/api/menu` | Speisekarte | 200 Array |
| POST | `/api/orders` | Bestellung anlegen | 201 `{order_id,status}`; 400, 503 |
| GET | `/api/orders/{id}` | Bestellung mit Positionen | 200; 400, 404 |
| PATCH | `/api/orders/{id}` | Status setzen (nur vorwärts) | 200 Bestellung oder `{ok:true}` bei paid; 400, 404 |
| GET | `/api/waiter/status` | Kellner beschäftigt? | 200 `{busy}` |
| POST | `/api/waiter/status` | `{"busy":true|false}` setzen | 200 `{ok:true}`; 400 |
| POST | `/api/waiter/call` | Kellner rufen `{table_number}` (1-20) | 201 neu / 200 vorhanden `{call_id}`; 400, 503 |
| GET | `/api/waiter/calls` | Offene Rufe | 200 Array |
| DELETE | `/api/waiter/calls/{id}` | Ruf erledigen | 200; 404 |
| GET | `/api/waiter/queue` | Offene Bestellungen (ohne paid) | 200 Array |
| GET | `/cert.pem` | Server-Zertifikat | Datei |
| OPTIONS | beliebig | CORS-Preflight | 204 |

Statische Dateien: `/`, `/customer`, `/waiter` und alle Dateien aus `frontend/`.

### POST /api/orders

```json
{"table_number":3,"customer_name":"optional","notes":"optional","items":[{"menu_item_id":1,"quantity":2}]}
```

Regeln: Tisch 1-20, 1-20 Positionen, Menge 1-99, `menu_item_id` muss existieren. `customer_name` max. 63 Zeichen, `notes` max. 255 (werden gekürzt).

### PATCH /api/orders/{id}

```json
{"status":"accepted"}
```

Gültig: `pending`, `accepted`, `preparing`, `ready`, `delivered`, `bill`, `paid`. Rückwärts oder gleicher Status ergibt 400.

## WebSocket

Endpunkt: `wss://<host>:8443/ws/notifications[?role=waiter][&table=N]`. Maximal 64 Clients.

Server-Events (Broadcast an alle Clients):

| `type` | Felder |
|---|---|
| `new_order` | `order_id`, `table_number`, `status` |
| `order_update` | `order_id`, `status` |
| `call_waiter` | `call_id`, `table_number` |
| `call_resolved` | `call_id`, `table_number` |
| `waiter_status` | `busy` |

Videocall-Signaling (`call_ring`, accept/reject, `call_end`, SDP/ICE) wird über denselben WebSocket gezielt an Kellner bzw. Tisch geleitet (`ws_send_to`). Die genauen Nachrichten stehen in `ws.c`/`webrtc.js` und wurden hier nicht ausgewertet (Annahme).

## Interne C-Schnittstellen

`include/logic.h`: `logic_place_order`, `logic_update_order`, `logic_call_waiter`, `logic_resolve_call`, `logic_set_availability` sowie Lesefunktionen `logic_menu_items`, `logic_order_detail`, `logic_active_order_for_table`, `logic_queue_orders`, `logic_active_calls`. Fehlercodes: `LOGIC_INVALID` (-1), `LOGIC_CAPACITY` (-2), `LOGIC_NOTFOUND` (-3), `LOGIC_BACKWARD` (-4).

Beispiel:

```bash
curl -k https://localhost:8443/api/menu
curl -k -X POST https://localhost:8443/api/orders -H 'Content-Type: application/json' -d '{"table_number":3,"items":[{"menu_item_id":1,"quantity":2}]}'
```
