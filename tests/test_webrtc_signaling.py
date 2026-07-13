#!/usr/bin/env python3
# python3 tests/test_webrtc_signaling.py
# pip3 install websockets, server muss laufen (TLS 8443)
#
# testet den webrtc-relay: offer/answer/ice, waiter-ring per tischnr, disconnect/reconnect im call
# C-server ist nur dummer relay. timeouts solange er offer/answer/ice noch nicht durchreicht (red-first)

# erst checken ob websockets ueberhaupt da ist, sonst gleich mit klarer meldung raus
try:
    import websockets
except ImportError:
    print("ERROR: run pip3 install websockets")
    raise SystemExit(1)

import asyncio
import json
import ssl

# self-signed localhost-cert -> CERT_NONE, sonst wirft ssl nur fehler
SSLCTX = ssl.create_default_context()
SSLCTX.check_hostname = False
SSLCTX.verify_mode = ssl.CERT_NONE

WS = "wss://localhost:8443/ws/notifications"  # wss:// weil TLS auf 8443, relay ueber /ws/notifications

PASS = 0
FAIL = 0


# offer geht kunde -> kellner, aber nur wenn der call schon aktiv ist
async def test_webrtc_offer_routed():
    """Verify webrtc_offer is relayed from customer to waiter.

    Establishes an active call session (customer rings -> waiter accepts -> customer
    receives call_accept), then customer sends webrtc_offer and asserts waiter receives it.
    """
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # erst call aktiv machen (kunde ruft an)
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                # kellner muss den ring kriegen, sonst setup kaputt
                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_webrtc_offer_routed — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_offer_routed — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept", "table_number": 3}))

                # kunde kriegt accept -> ACTIVE, ab jetzt relayed der server offer/answer/ice
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_webrtc_offer_routed — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_offer_routed — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # jetzt der test: kunde schickt offer, kellner muss es kriegen
                await customer_ws.send(json.dumps({"type": "webrtc_offer", "sdp": {"type": "offer", "sdp": "v=0\r\n"}}))
                try:
                    msg = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if msg.get("type") == "webrtc_offer":
                        print("PASS: test_webrtc_offer_routed — webrtc_offer forwarded to waiter")
                        PASS += 1
                    else:
                        print(f"FAIL: test_webrtc_offer_routed — waiter got {msg.get('type')!r} instead of webrtc_offer")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_offer_routed — waiter did not receive webrtc_offer within 2s")
                    FAIL += 1

                # aufraeumen, sonst blockt g_call_session den naechsten test mit busy
                try:
                    await customer_ws.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                except Exception:
                    pass  # egal ob's klappt

    except Exception as e:
        print(f"FAIL: test_webrtc_offer_routed — connection error: {e}")
        FAIL += 1


# gegenrichtung: answer geht kellner -> kunde
async def test_webrtc_answer_routed():
    """Verify webrtc_answer is relayed from waiter to customer.

    Establishes an active call session, then waiter sends webrtc_answer and asserts
    customer receives it.
    """
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # setup wie oben
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_webrtc_answer_routed — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_answer_routed — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept", "table_number": 3}))

                # kunde kriegt accept -> ACTIVE
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_webrtc_answer_routed — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_answer_routed — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner schickt answer, kunde muss es kriegen
                await waiter_ws.send(json.dumps({"type": "webrtc_answer", "sdp": {"type": "answer", "sdp": "v=0\r\n"}}))
                try:
                    msg = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if msg.get("type") == "webrtc_answer":
                        print("PASS: test_webrtc_answer_routed — webrtc_answer forwarded to customer")
                        PASS += 1
                    else:
                        print(f"FAIL: test_webrtc_answer_routed — customer got {msg.get('type')!r} instead of webrtc_answer")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_answer_routed — customer did not receive webrtc_answer within 2s")
                    FAIL += 1

                # aufraeumen, sonst haengt g_call_session
                try:
                    await customer_ws.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                except Exception:
                    pass  # nur best-effort drain

    except Exception as e:
        print(f"FAIL: test_webrtc_answer_routed — connection error: {e}")
        FAIL += 1


# ice in beide richtungen, ein einziges PASS deckt beide ab
async def test_webrtc_ice_routed():
    """Verify webrtc_ice candidates are relayed in both directions.

    Establishes an active call session, then tests ICE routing customer->waiter
    and waiter->customer. A single PASS/FAIL covers both directions.
    """
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # setup: call aktiv machen
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                # kellner muss call_ring kriegen, sonst setup kaputt
                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_webrtc_ice_routed — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_ice_routed — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept", "table_number": 3}))

                # kunde kriegt call_accept -> ACTIVE
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_webrtc_ice_routed — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_ice_routed — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # richtung 1: kunde -> kellner
                # fake host-candidate aus dem lan, reicht als payload
                await customer_ws.send(json.dumps({
                    "type": "webrtc_ice",
                    "candidate": {
                        "candidate": "candidate:1 1 UDP 2122252543 192.168.1.10 54321 typ host",
                        "sdpMLineIndex": 0
                    }
                }))
                try:
                    msg = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if msg.get("type") != "webrtc_ice":
                        print(f"FAIL: test_webrtc_ice_routed — waiter got {msg.get('type')!r} instead of webrtc_ice (customer->waiter direction)")
                        FAIL += 1
                        # richtung 1 kaputt, aufraeumen und raus
                        try:
                            await customer_ws.send(json.dumps({"type": "call_end"}))
                            await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                        except Exception:
                            pass
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_ice_routed — waiter did not receive webrtc_ice within 2s (customer->waiter direction)")
                    FAIL += 1
                    # timeout -> aufraeumen und raus
                    try:
                        await customer_ws.send(json.dumps({"type": "call_end"}))
                        await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                    except Exception:
                        pass
                    return

                # richtung 2: kellner -> kunde
                await waiter_ws.send(json.dumps({
                    "type": "webrtc_ice",
                    "candidate": {
                        "candidate": "candidate:2 1 UDP 2122252543 192.168.1.11 54322 typ host",
                        "sdpMLineIndex": 0
                    }
                }))
                try:
                    msg = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if msg.get("type") == "webrtc_ice":
                        print("PASS: test_webrtc_ice_routed — webrtc_ice routed in both directions")
                        PASS += 1
                    else:
                        print(f"FAIL: test_webrtc_ice_routed — customer got {msg.get('type')!r} instead of webrtc_ice (waiter->customer direction)")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_webrtc_ice_routed — customer did not receive webrtc_ice within 2s (waiter->customer direction)")
                    FAIL += 1

                # aufraeumen fuer den naechsten test
                try:
                    await customer_ws.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                except Exception:
                    pass  # nur best-effort drain

    except Exception as e:
        print(f"FAIL: test_webrtc_ice_routed — connection error: {e}")
        FAIL += 1


# kellner ruft den kunden an per tischnr, braucht keine aktive session
async def test_waiter_initiated_ring():
    """Verify waiter-initiated call_ring is routed to the correct customer table.

    No prior call session needed — this tests call setup. Waiter sends call_ring
    targeting table 5; customer at table 5 must receive it (CALL-03).
    """
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=5", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # kellner ruft tisch 5 an
                await waiter_ws.send(json.dumps({"type": "call_ring", "table_number": 5}))

                # tisch 5 muss den ring kriegen, server routet per tischnr
                try:
                    msg = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if msg.get("type") == "call_ring":
                        print("PASS: test_waiter_initiated_ring — call_ring routed from waiter to customer table 5")
                        PASS += 1
                    else:
                        print(f"FAIL: test_waiter_initiated_ring — customer got {msg.get('type')!r} instead of call_ring")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_waiter_initiated_ring — customer did not receive call_ring within 2s")
                    FAIL += 1

                # aufraeumen, best-effort
                try:
                    await waiter_ws.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(customer_ws.recv(), timeout=0.5)
                except Exception:
                    pass  # nur best-effort drain

    except Exception as e:
        print(f"FAIL: test_waiter_initiated_ring — connection error: {e}")
        FAIL += 1


# kunde macht tab zu mitten im call -> server muss dem kellner call_end schicken
# sonst haengt die session ewig auf busy
async def test_disconnect_mid_call():
    """STAB-01: customer tab close mid-call causes waiter to receive call_end within 5s."""
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # setup: call aktiv machen
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                # kellner muss call_ring kriegen, sonst setup kaputt
                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_disconnect_mid_call — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_disconnect_mid_call — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept"}))

                # kunde kriegt accept -> ACTIVE
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_disconnect_mid_call — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_disconnect_mid_call — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # kunde verlaesst das innere with -> FIN -> ws_remove feuert (tab-close simuliert)

            # jetzt muss der kellner von selbst call_end kriegen, 5s reicht auch aufm pi
            try:
                msg = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=5.0))
                if msg.get("type") == "call_end":
                    print("PASS: test_disconnect_mid_call — waiter received call_end after customer disconnect")
                    PASS += 1
                else:
                    print(f"FAIL: test_disconnect_mid_call — waiter got {msg.get('type')!r} instead of call_end")
                    FAIL += 1
            except asyncio.TimeoutError:
                print("FAIL: test_disconnect_mid_call — no call_end within 5s (stuck busy)")
                FAIL += 1

    except Exception as e:
        print(f"FAIL: test_disconnect_mid_call — connection error: {e}")
        FAIL += 1


# andersrum: kellner geht mitten im call -> kunde muss call_end kriegen
# customer aussen, waiter innen, damit der waiter zuerst dicht macht
async def test_waiter_disconnect_mid_call():
    """STAB-01 symmetric: waiter tab close mid-call causes customer to receive call_end within 5s."""
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
            async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
                # setup: call aktiv machen
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                # kellner muss call_ring kriegen, sonst setup kaputt
                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_waiter_disconnect_mid_call — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_waiter_disconnect_mid_call — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept"}))

                # kunde kriegt call_accept -> ACTIVE
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_waiter_disconnect_mid_call — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_waiter_disconnect_mid_call — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner-with geht zu -> FIN -> ws_remove feuert

            # kunde muss jetzt call_end kriegen (spiegelbild)
            try:
                msg = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=5.0))
                if msg.get("type") == "call_end":
                    print("PASS: test_waiter_disconnect_mid_call — customer received call_end after waiter disconnect")
                    PASS += 1
                else:
                    print(f"FAIL: test_waiter_disconnect_mid_call — customer got {msg.get('type')!r} instead of call_end")
                    FAIL += 1
            except asyncio.TimeoutError:
                print("FAIL: test_waiter_disconnect_mid_call — no call_end within 5s (stuck busy)")
                FAIL += 1

    except Exception as e:
        print(f"FAIL: test_waiter_disconnect_mid_call — connection error: {e}")
        FAIL += 1


# nach disconnect + call_end muss ein neuer call durchgehen, nicht an busy scheitern
# also: wurde g_call_session wirklich geleert?
async def test_reconnect_after_disconnect():
    """STAB-01: after customer disconnect + call_end, a new call_ring is accepted (not call_busy)."""
    global PASS, FAIL
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # setup: ersten call aktiv machen
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                # kellner muss call_ring kriegen, sonst setup kaputt
                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_reconnect_after_disconnect — waiter got {ring.get('type')!r} instead of call_ring (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_reconnect_after_disconnect — waiter did not receive call_ring within 2s (setup failed)")
                    FAIL += 1
                    return

                # kellner nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept"}))

                # kunde kriegt accept -> ACTIVE
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") != "call_accept":
                        print(f"FAIL: test_reconnect_after_disconnect — customer got {accept.get('type')!r} instead of call_accept (setup failed)")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_reconnect_after_disconnect — customer did not receive call_accept within 2s (setup failed)")
                    FAIL += 1
                    return

                # with zu -> FIN, ws_remove feuert, g_call_session sollte leer sein

            # das call_end vom server einmal wegschlucken
            try:
                await asyncio.wait_for(waiter_ws.recv(), timeout=5.0)
            except Exception:
                pass  # kommt keins, egal, uns interessiert nur der reconnect

            # reconnect: neuer kunde an tisch 3 ruft neu an
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws2:
                await customer_ws2.send(json.dumps({"type": "call_ring", "table_number": 3}))

                try:
                    ring2 = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring2.get("type") == "call_ring":
                        print("PASS: test_reconnect_after_disconnect — new call accepted after disconnect")
                        PASS += 1
                    else:
                        print(f"FAIL: test_reconnect_after_disconnect — waiter got {ring2.get('type')!r} instead of call_ring (call_busy indicates g_call_session not cleared)")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_reconnect_after_disconnect — no response to new call_ring within 2s")
                    FAIL += 1

                # aufraeumen
                try:
                    await customer_ws2.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                except Exception:
                    pass

    except Exception as e:
        print(f"FAIL: test_reconnect_after_disconnect — connection error: {e}")
        FAIL += 1


if __name__ == "__main__":
    async def main():
        # nacheinander, nicht parallel, sonst kollidieren sie auf g_call_session
        await test_webrtc_offer_routed()
        await test_webrtc_answer_routed()
        await test_webrtc_ice_routed()
        await test_waiter_initiated_ring()
        await test_disconnect_mid_call()
        await test_waiter_disconnect_mid_call()
        await test_reconnect_after_disconnect()

    asyncio.run(main())

    print(f"\nResults: {PASS} passed, {FAIL} failed")
    raise SystemExit(0 if FAIL == 0 else 1)
