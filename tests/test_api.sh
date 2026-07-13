#!/usr/bin/env bash
# start: bash tests/test_api.sh
# server muss vorher laufen (./web_server aus dem projekt-root)
set -e
BASE="https://localhost:8443"
PASS=0
FAIL=0

# schaut ob $expected irgendwo in $actual steht, substring reicht
check() {
  local label="$1" expected="$2" actual="$3"
  if echo "$actual" | grep -q "$expected"; then
    echo "PASS: $label"
    # ((PASS++)) gibt bei PASS=0 nen exit 1 (post-increment liefert den alten wert), || true rettet uns vor set -e
    ((PASS++)) || true
  else
    echo "FAIL: $label (expected '$expected' in: $actual)"
    ((FAIL++)) || true
  fi
}

# wie check nur exakt statt substring, fuer die status codes
check_status() {
  local label="$1" expected="$2" actual="$3"
  if [ "$actual" = "$expected" ]; then
    echo "PASS: $label (HTTP $actual)"
    ((PASS++)) || true
  else
    echo "FAIL: $label (expected HTTP $expected, got $actual)"
    ((FAIL++)) || true
  fi
}

# menu muss was mit "name" liefern, sonst ist die karte kaputt
# -k weil self-signed, -s damit die progressbar nicht in die response rutscht
R=$(curl -k -s "$BASE/api/menu")
check "GET /api/menu returns name field" '"name"' "$R"
# echt durch json.tool jagen, nicht nur zufaellig das wort "name" getroffen
echo "$R" | python3 -m json.tool > /dev/null 2>&1 \
  && { echo "PASS: GET /api/menu is valid JSON"; ((PASS++)) || true; } \
  || { echo "FAIL: GET /api/menu is not valid JSON"; ((FAIL++)) || true; }

# happy path: order anlegen, will 201 + order_id
# body in tmp-file, status-code per -w abgreifen
HTTP_RESP=$(curl -k -s -o /tmp/order_resp.json -w "%{http_code}" \
  -X POST "$BASE/api/orders" \
  -H "Content-Type: application/json" \
  -d '{"table_number":3,"items":[{"menu_item_id":1,"quantity":2}]}')
R=$(cat /tmp/order_resp.json)
check_status "POST /api/orders returns HTTP 201" "201" "$HTTP_RESP"
check "POST /api/orders returns order_id" '"order_id"' "$R"
# order_id fuer die folge-tests rausziehen, bash kann kein json
ORDER_ID=$(echo "$R" | python3 -c "import sys,json; print(json.load(sys.stdin)['order_id'])" 2>/dev/null || echo "")
if [ -z "$ORDER_ID" ]; then
  echo "FAIL: Could not parse order_id from response"
  ((FAIL++)) || true
else
  echo "PASS: Parsed order_id=$ORDER_ID"
  ((PASS++)) || true
fi

R=$(curl -k -s "$BASE/api/orders/$ORDER_ID")
check "GET /api/orders/:id returns status field" '"status"' "$R"
check "GET /api/orders/:id status is pending" '"pending"' "$R"

# kellner nimmt an, pending -> accepted
R=$(curl -k -s -X PATCH "$BASE/api/orders/$ORDER_ID" \
  -H "Content-Type: application/json" \
  -d '{"status":"accepted"}')
check "PATCH /api/orders/:id transitions to accepted" '"accepted"' "$R"

R=$(curl -k -s -X POST "$BASE/api/waiter/status" \
  -H "Content-Type: application/json" \
  -d '{"busy":true}')
check "POST /api/waiter/status returns ok" '"ok"' "$R"

# queue muss ein array sein (nicht object/null) sonst kotzt das dashboard
R=$(curl -k -s "$BASE/api/waiter/queue")
if echo "$R" | python3 -c "import sys,json; d=json.load(sys.stdin); assert isinstance(d,list)" > /dev/null 2>&1; then
  echo "PASS: GET /api/waiter/queue returns JSON array"
  ((PASS++)) || true
else
  echo "FAIL: GET /api/waiter/queue did not return array (got: $R)"
  ((FAIL++)) || true
fi

# muell-input darf nicht crashen, kaputtes json muss sauber 400 geben
STATUS=$(curl -k -s -o /dev/null -w "%{http_code}" \
  -X POST "$BASE/api/orders" \
  -H "Content-Type: application/json" \
  -d 'not json')
check_status "POST /api/orders malformed JSON returns 400" "400" "$STATUS"

# table_number fehlt -> 400, sonst haetten wir orders ohne tisch
STATUS=$(curl -k -s -o /dev/null -w "%{http_code}" \
  -X POST "$BASE/api/orders" \
  -H "Content-Type: application/json" \
  -d '{"items":[]}')
check_status "POST /api/orders missing table_number returns 400" "400" "$STATUS"

# tisch 99 gibts nicht, validierung muss greifen
STATUS=$(curl -k -s -o /dev/null -w "%{http_code}" \
  -X POST "$BASE/api/orders" \
  -H "Content-Type: application/json" \
  -d '{"table_number":99,"items":[{"menu_item_id":1,"quantity":1}]}')
if [ "$STATUS" = "400" ]; then
  echo "PASS: POST /api/orders table_number out of range returns 400 (HTTP 400)"
  ((PASS++)) || true
else
  echo "FAIL: POST /api/orders table_number out of range (expected HTTP 400, got $STATUS)"
  ((FAIL++)) || true
fi

# nach dem anlegen oben muss die queue jetzt eintraege haben
R=$(curl -k -s "$BASE/api/waiter/queue")
if echo "$R" | python3 -c "import sys,json; d=json.load(sys.stdin); assert len(d)>0" > /dev/null 2>&1; then
  echo "PASS: GET /api/waiter/queue has entries after order"
  ((PASS++)) || true
else
  echo "FAIL: GET /api/waiter/queue is empty after order creation"
  ((FAIL++)) || true
fi

# existierende order gibt 200, nicht 404
OSTATUS=$(curl -k -s -o /dev/null -w "%{http_code}" "$BASE/api/orders/$ORDER_ID")
if [ "$OSTATUS" = "200" ]; then
  echo "PASS: GET /api/orders/:id returns HTTP 200"
  ((PASS++)) || true
else
  echo "FAIL: GET /api/orders/:id expected HTTP 200, got $OSTATUS"
  ((FAIL++)) || true
fi

# waiter-status braucht ein busy-feld damit der kunde sieht ob frei
R=$(curl -k -s "$BASE/api/waiter/status")
check "GET /api/waiter/status returns busy field" '"busy"' "$R"

# nur zur info wie flott menu ist, zaehlt nicht in PASS/FAIL
echo ""
echo "NFR-1 timing check (informational — does not affect PASS/FAIL):"
time curl -k -s "$BASE/api/menu" > /dev/null
echo ""

echo "Results: $PASS passed, $FAIL failed"
# exit 1 wenn was failed damit der aufrufer / CI das merkt
[ "$FAIL" -eq 0 ] || exit 1
