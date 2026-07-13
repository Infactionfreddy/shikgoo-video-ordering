#!/usr/bin/env python3
# python3 tests/test_ws.py
# server muss laufen (./web_server im projekt-root)
# pip3 install websockets
# plain ws auf 8080, nicht die TLS-variante auf 8443

# websockets da? sonst klare meldung statt kryptischem traceback
try:
    import websockets
except ImportError:
    print("ERROR: run pip3 install websockets")
    raise SystemExit(1)

import asyncio
import json
import urllib.request

BASE = "http://localhost:8080"
WS   = "ws://localhost:8080/ws/notifications"  # ws:// = plain, kein TLS auf 8080

PASS = 0
FAIL = 0


async def test_ws():
    global PASS, FAIL

    # kommt der ws-upgrade ueberhaupt zustande?
    try:
        async with websockets.connect(WS, open_timeout=5) as ws:
            print("PASS: WebSocket connection upgraded successfully")
            PASS += 1

            # order per POST rein, kommt der live-push ueber den ws?
            # 3s timeout ist grosszuegig, das sollte quasi sofort da sein
            req = urllib.request.Request(
                f"{BASE}/api/orders",
                data=json.dumps({
                    "table_number": 5,
                    "items": [{"menu_item_id": 1, "quantity": 1}]
                }).encode(),
                headers={"Content-Type": "application/json"},
                method="POST"
            )
            with urllib.request.urlopen(req) as resp:
                order_data = json.load(resp)

            order_id = order_data["order_id"]
            print(f"  Placed order_id={order_id}, awaiting WebSocket push...")

            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=3.0)
                msg = json.loads(raw)

                # fehlt type oder order_id ist der broadcast kaputt
                if "type" not in msg or "order_id" not in msg:
                    print(f"FAIL: Push message missing 'type' or 'order_id' fields (got: {msg})")
                    FAIL += 1
                elif msg["order_id"] != order_id:
                    print(f"FAIL: Push order_id mismatch (expected {order_id}, got {msg['order_id']})")
                    FAIL += 1
                else:
                    print(f"PASS: WebSocket push received for order_id={order_id} (type={msg['type']!r})")
                    PASS += 1

            except asyncio.TimeoutError:
                print("FAIL: No WebSocket push received within 3 seconds of POST /api/orders")
                FAIL += 1

    except Exception as e:
        print(f"FAIL: WebSocket connection error: {e}")
        FAIL += 1


if __name__ == "__main__":
    asyncio.run(test_ws())

    print(f"\nResults: {PASS} passed, {FAIL} failed")
    raise SystemExit(0 if FAIL == 0 else 1)
