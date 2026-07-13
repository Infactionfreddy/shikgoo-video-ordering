#!/usr/bin/env bash
# start: bash tests/test_tls.sh
# server muss vorher laufen (./web_server aus projekt-root)
# braucht curl, openssl und python3
set -e
BASE="https://localhost:8443"
PASS=0
FAIL=0

# substring-check, wie in test_api.sh
check() {
  local label="$1" expected="$2" actual="$3"
  if echo "$actual" | grep -q "$expected"; then
    echo "PASS: $label"
    # || true weil ((PASS++)) bei 0 nen non-zero exit gibt, sonst killt set -e uns
    ((PASS++)) || true
  else
    echo "FAIL: $label (expected '$expected' in: $actual)"
    ((FAIL++)) || true
  fi
}

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

# kommt ueberhaupt ne TLS-verbindung zustande und liegt sauberes json drueber?
# -k weil self-signed
R=$(curl -k -s "$BASE/api/menu")
echo "$R" | python3 -m json.tool > /dev/null 2>&1 \
  && { echo "PASS: GET /api/menu returns valid JSON over HTTPS"; ((PASS++)) || true; } \
  || { echo "FAIL: GET /api/menu not valid JSON over HTTPS (got: $R)"; ((FAIL++)) || true; }

# ist es auch wirklich unser cert? -v spuckt die cert-details aus, wir grepen nach shikgoo
# -I reicht (head request) um an die handshake-infos zu kommen
CERT_INFO=$(curl -kvI "$BASE/" 2>&1)
if echo "$CERT_INFO" | grep -qi "shikgoo"; then
  echo "PASS: TLS cert subject contains shikgoo"
  ((PASS++)) || true
else
  echo "FAIL: TLS cert subject does not contain shikgoo"
  ((FAIL++)) || true
fi

# SAN muss shikgoo.local enthalten, sonst meckern die browser trotz gueltigem cert
# cert per openssl ziehen und den textdump nach dem hostname absuchen
SAN=$(openssl s_client -connect localhost:8443 </dev/null 2>/dev/null | openssl x509 -noout -text 2>/dev/null)
if echo "$SAN" | grep -q "shikgoo.local"; then
  echo "PASS: Cert SAN contains shikgoo.local"
  ((PASS++)) || true
else
  echo "FAIL: Cert SAN does not contain shikgoo.local"
  ((FAIL++)) || true
fi

# plain HTTP auf 8080 muss tot sein, wir sind https-only
# connect muss failen (refused/timeout), antwortet da was ist es ein fail
HTTP_RESULT=$(curl -sS --connect-timeout 2 "http://localhost:8080/api/menu" 2>&1 || true)
if echo "$HTTP_RESULT" | grep -qiE "refused|timed out|couldn't connect|Failed to connect"; then
  echo "PASS: No plain-HTTP listener on port 8080"
  ((PASS++)) || true
else
  echo "FAIL: Port 8080 responded — plain HTTP should not be running (got: $HTTP_RESULT)"
  ((FAIL++)) || true
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
# non-zero exit wenn was schiefging
[ "$FAIL" -eq 0 ] || exit 1
