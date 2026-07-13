#!/usr/bin/env python3
# python3 tests/test_ws_signaling.py
# server muss laufen, TLS auf 8443
# pip3 install websockets
#
# testet das call-signaling: ring/accept/reject/busy/end
# solange die C-seite das nicht kann laufen die tests in timeouts, am anfang so gewollt

# websockets installiert? sonst gleich mit klarer meldung raus
try:
    import websockets
except ImportError:
    print("ERROR: run pip3 install websockets")
    raise SystemExit(1)

import asyncio
import json
import ssl

# self-signed localhost-cert, drum hostname-check aus + CERT_NONE
# sonst lehnt python wegen "untrusted cert" ab
SSLCTX = ssl.create_default_context()
SSLCTX.check_hostname = False
SSLCTX.verify_mode = ssl.CERT_NONE

WS = "wss://localhost:8443/ws/notifications"  # wss:// weil TLS auf 8443

PASS = 0
FAIL = 0


async def test_waiter_isolation():
    """SIG-01 role registration + SIG-02 unicast routing.

    Connect waiter + customer + observer. Customer sends call_ring.
    Assert: waiter receives it; observer receives nothing.
    """
    global PASS, FAIL
    # call_ring geht NUR an den waiter, der observer auf table=99 darf nix kriegen
    try:
        # rolle per query-string: role=waiter vs table=N
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                async with websockets.connect(WS + "?table=99", ssl=SSLCTX, open_timeout=5) as observer_ws:
                    await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                    # waiter muss den ring kriegen, 2s reicht im lan
                    try:
                        msg = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                        if msg.get("type") == "call_ring":
                            print("PASS: test_waiter_isolation — call_ring routed to waiter")
                            PASS += 1
                        else:
                            print(f"FAIL: test_waiter_isolation — waiter got unexpected type {msg.get('type')!r}")
                            FAIL += 1
                    except asyncio.TimeoutError:
                        print("FAIL: test_waiter_isolation — waiter did not receive call_ring within 2s")
                        FAIL += 1

                    # observer darf nix kriegen, timeout ist hier der PASS-fall
                    try:
                        unexpected = json.loads(await asyncio.wait_for(observer_ws.recv(), timeout=1.0))
                        print(f"FAIL: test_waiter_isolation — observer received unexpected message: {unexpected}")
                        FAIL += 1
                    except asyncio.TimeoutError:
                        print("PASS: test_waiter_isolation — observer correctly received nothing")
                        PASS += 1

    except Exception as e:
        print(f"FAIL: test_waiter_isolation — connection error: {e}")
        FAIL += 1


async def test_table_routing():
    """SIG-01 table registration + SIG-02 route-back to table.

    Connect waiter + customer at table 3.
    Customer sends call_ring; waiter receives it.
    Waiter sends call_accept for table 3; customer at table 3 must receive it.
    """
    global PASS, FAIL
    # rueckrichtung waiter -> customer: das accept muss an tisch 3, nicht broadcast
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as customer_ws:
                # customer klingelt durch
                await customer_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))

                try:
                    ring = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                    if ring.get("type") != "call_ring":
                        print(f"FAIL: test_table_routing — waiter got {ring.get('type')!r} instead of call_ring")
                        FAIL += 1
                        return
                except asyncio.TimeoutError:
                    print("FAIL: test_table_routing — waiter did not receive call_ring within 2s")
                    FAIL += 1
                    return

                # waiter nimmt an
                await waiter_ws.send(json.dumps({"type": "call_accept", "table_number": 3}))

                # accept muss zurueck an tisch 3
                try:
                    accept = json.loads(await asyncio.wait_for(customer_ws.recv(), timeout=2.0))
                    if accept.get("type") == "call_accept":
                        print("PASS: test_table_routing — call_accept routed back to table 3")
                        PASS += 1
                    else:
                        print(f"FAIL: test_table_routing — customer got {accept.get('type')!r} instead of call_accept")
                        FAIL += 1
                except asyncio.TimeoutError:
                    print("FAIL: test_table_routing — customer did not receive call_accept within 2s")
                    FAIL += 1

                # aufraeumen: call_end, sonst haengt g_call_session und der naechste test kriegt busy
                try:
                    await customer_ws.send(json.dumps({"type": "call_end"}))
                    await asyncio.wait_for(waiter_ws.recv(), timeout=0.5)
                except Exception:
                    pass  # nur best-effort drain, egal wenn nix mehr kommt

    except Exception as e:
        print(f"FAIL: test_table_routing — connection error: {e}")
        FAIL += 1


async def test_call_busy():
    """SIG-03 call state tracking + SIG-04 busy gating.

    Sequence:
      c3 sends call_ring → waiter receives it.
      waiter sends call_accept → c3 receives it (call now ACTIVE).
      c4 sends call_ring → c4 must receive call_busy.
      c3 sends call_end → waiter receives call_end (best-effort, non-fatal if absent).
      c4 sends call_ring again → waiter must receive it (proves g_call_session cleared).
    """
    global PASS, FAIL
    # waehrend ein call laeuft muss c4 busy kriegen statt durchgestellt zu werden
    # nach call_end ist die session frei und c4 kommt beim naechsten ring durch
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as c3_ws:
                async with websockets.connect(WS + "?table=4", ssl=SSLCTX, open_timeout=5) as c4_ws:
                    # c3 klingelt
                    await c3_ws.send(json.dumps({"type": "call_ring", "table_number": 3}))
                    try:
                        await asyncio.wait_for(waiter_ws.recv(), timeout=2.0)  # waiter kriegt den ring
                    except asyncio.TimeoutError:
                        print("FAIL: test_call_busy — waiter did not receive initial call_ring within 2s")
                        FAIL += 1
                        return

                    # waiter nimmt an -> call ist jetzt ACTIVE
                    await waiter_ws.send(json.dumps({"type": "call_accept", "table_number": 3}))
                    try:
                        await asyncio.wait_for(c3_ws.recv(), timeout=2.0)  # c3 kriegt das accept
                    except asyncio.TimeoutError:
                        print("FAIL: test_call_busy — c3 did not receive call_accept within 2s")
                        FAIL += 1
                        return

                    # c4 klingelt mitten im call, muss busy kriegen
                    await c4_ws.send(json.dumps({"type": "call_ring", "table_number": 4}))
                    try:
                        busy = json.loads(await asyncio.wait_for(c4_ws.recv(), timeout=2.0))
                        if busy.get("type") == "call_busy":
                            print("PASS: test_call_busy — c4 received call_busy while call active")
                            PASS += 1
                        else:
                            print(f"FAIL: test_call_busy — c4 got {busy.get('type')!r} instead of call_busy")
                            FAIL += 1
                    except asyncio.TimeoutError:
                        print("FAIL: test_call_busy — c4 did not receive call_busy within 2s")
                        FAIL += 1

                    # c3 legt auf, ob der waiter das call_end sieht ist nebensache
                    await c3_ws.send(json.dumps({"type": "call_end"}))
                    try:
                        end_msg = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                        print(f"  [info] waiter received after call_end: type={end_msg.get('type')!r}")
                    except asyncio.TimeoutError:
                        print("  [info] waiter did not receive call_end within 2s (non-fatal)")

                    # beweis: nach dem end geht der c4-ring wieder an den waiter durch
                    await c4_ws.send(json.dumps({"type": "call_ring", "table_number": 4}))
                    try:
                        ring2 = json.loads(await asyncio.wait_for(waiter_ws.recv(), timeout=2.0))
                        if ring2.get("type") == "call_ring":
                            print("PASS: test_call_busy — call_end cleared session; c4 ring forwarded to waiter")
                            PASS += 1
                        else:
                            print(f"FAIL: test_call_busy — waiter got {ring2.get('type')!r} after call_end; session may not have cleared")
                            FAIL += 1
                    except asyncio.TimeoutError:
                        print("FAIL: test_call_busy — waiter did not receive second call_ring within 2s (session not cleared?)")
                        FAIL += 1

    except Exception as e:
        print(f"FAIL: test_call_busy — connection error: {e}")
        FAIL += 1


async def test_registry_logging():
    """SIG-01 registry — connection smoke test.

    Connect as ?role=waiter and ?table=5. Assert both connections open without
    exception. Server-side fd/table/role logging is verified visually during
    test run per 02-RESEARCH.md Success Criterion 3.
    """
    global PASS, FAIL
    # smoke: gehen waiter- und tisch-connect sauber auf?
    # das fd/table/role-logging serverseitig guckt man beim run mit augen nach
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter_ws:
            async with websockets.connect(WS + "?table=5", ssl=SSLCTX, open_timeout=5) as table5_ws:
                # beide offen reicht, kein recv noetig
                _ = waiter_ws
                _ = table5_ws
        print("PASS: test_registry_logging — waiter and table=5 connections opened successfully")
        PASS += 1
    except Exception as e:
        print(f"FAIL: test_registry_logging — connection error: {e}")
        FAIL += 1


async def test_full_call_sequence():
    """Full 9-step call sequence: ring -> accept -> busy (x2) -> end -> ring again.

    SIG-01/02/03/04 end-to-end integration — proves the complete call state machine:
      Step 1: waiter connects (?role=waiter)
      Step 2: customer table 3 connects (?table=3)
      Step 3: customer table 4 connects (?table=4)
      Step 4: c3 sends call_ring    -> waiter receives call_ring
      Step 5: waiter sends call_accept(3) -> c3 receives call_accept (call ACTIVE)
      Step 6: c4 sends call_ring    -> c4 receives call_busy
      Step 7: c4 sends call_ring again -> c4 receives call_busy (idempotent)
      Step 8: c3 sends call_end     -> waiter receives call_end (session cleared)
      Step 9: c4 sends call_ring again -> waiter receives call_ring (NOT busy; proves cleared)
    """
    global PASS, FAIL
    # ganzer durchlauf am stueck: ring -> accept -> busy -> busy -> end -> ring geht wieder durch
    try:
        async with websockets.connect(WS + "?role=waiter", ssl=SSLCTX, open_timeout=5) as waiter:
            async with websockets.connect(WS + "?table=3", ssl=SSLCTX, open_timeout=5) as c3:
                async with websockets.connect(WS + "?table=4", ssl=SSLCTX, open_timeout=5) as c4:

                    # c3 klingelt -> waiter muss den ring sehen
                    await c3.send(json.dumps({"type": "call_ring", "table_number": 3}))
                    try:
                        msg = json.loads(await asyncio.wait_for(waiter.recv(), timeout=2.0))
                        if msg.get("type") == "call_ring":
                            print("PASS: test_full_call_sequence — step 4: waiter received call_ring from c3")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 4: waiter got {msg.get('type')!r} instead of call_ring")
                            FAIL += 1
                            return
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 4: waiter did not receive call_ring within 2s")
                        FAIL += 1
                        return

                    # waiter nimmt an -> c3 kriegt das accept, call ACTIVE
                    await waiter.send(json.dumps({"type": "call_accept", "table_number": 3}))
                    try:
                        msg = json.loads(await asyncio.wait_for(c3.recv(), timeout=2.0))
                        if msg.get("type") == "call_accept":
                            print("PASS: test_full_call_sequence — step 5: c3 received call_accept (call ACTIVE)")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 5: c3 got {msg.get('type')!r} instead of call_accept")
                            FAIL += 1
                            return
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 5: c3 did not receive call_accept within 2s")
                        FAIL += 1
                        return

                    # c4 klingelt in den call rein -> muss busy kriegen
                    await c4.send(json.dumps({"type": "call_ring", "table_number": 4}))
                    try:
                        msg = json.loads(await asyncio.wait_for(c4.recv(), timeout=2.0))
                        if msg.get("type") == "call_busy":
                            print("PASS: test_full_call_sequence — step 6: c4 received call_busy (call in progress)")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 6: c4 got {msg.get('type')!r} instead of call_busy")
                            FAIL += 1
                            return  # beim ersten fail raus, rest waer nur folgefehler
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 6: c4 did not receive call_busy within 2s")
                        FAIL += 1
                        return

                    # c4 nochmal -> immer noch busy, darf nicht ploetzlich durchgehen
                    await c4.send(json.dumps({"type": "call_ring", "table_number": 4}))
                    try:
                        msg = json.loads(await asyncio.wait_for(c4.recv(), timeout=2.0))
                        if msg.get("type") == "call_busy":
                            print("PASS: test_full_call_sequence — step 7: c4 received call_busy again (idempotent)")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 7: c4 got {msg.get('type')!r} instead of call_busy (idempotent check)")
                            FAIL += 1
                            return
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 7: c4 did not receive second call_busy within 2s")
                        FAIL += 1
                        return

                    # c3 legt auf -> waiter kriegt call_end, session wird geleert
                    await c3.send(json.dumps({"type": "call_end"}))
                    try:
                        msg = json.loads(await asyncio.wait_for(waiter.recv(), timeout=2.0))
                        if msg.get("type") == "call_end":
                            print("PASS: test_full_call_sequence — step 8: waiter received call_end (session cleared)")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 8: waiter got {msg.get('type')!r} instead of call_end")
                            FAIL += 1
                            return  # ohne call_end macht der letzte schritt keinen sinn
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 8: waiter did not receive call_end within 2s")
                        FAIL += 1
                        return

                    # c4 erneut -> muss jetzt durchgehen, sonst wurde die session nicht frei
                    await c4.send(json.dumps({"type": "call_ring", "table_number": 4}))
                    try:
                        msg = json.loads(await asyncio.wait_for(waiter.recv(), timeout=2.0))
                        if msg.get("type") == "call_ring":
                            print("PASS: test_full_call_sequence — step 9: ring again succeeded after call_end (g_call_session cleared)")
                            PASS += 1
                        else:
                            print(f"FAIL: test_full_call_sequence — step 9: waiter got {msg.get('type')!r} instead of call_ring (session not cleared?)")
                            FAIL += 1
                            return
                    except asyncio.TimeoutError:
                        print("FAIL: test_full_call_sequence — step 9: waiter did not receive call_ring after call_end within 2s (session not cleared?)")
                        FAIL += 1

    except Exception as e:
        print(f"FAIL: test_full_call_sequence — connection error: {e}")
        FAIL += 1


if __name__ == "__main__":
    async def main():
        await test_waiter_isolation()
        await test_table_routing()
        await test_call_busy()
        await test_registry_logging()
        await test_full_call_sequence()

    asyncio.run(main())

    print(f"\nResults: {PASS} passed, {FAIL} failed")
    raise SystemExit(0 if FAIL == 0 else 1)
