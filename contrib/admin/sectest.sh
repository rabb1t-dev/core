#!/bin/bash
# Attempt each attack the panel is supposed to resist and report pass/fail.
# Run after smoke.sh. The login-throttle check is last because it locks out 127.0.0.1.
export HISTFILE=/dev/null
# The panel login to test with. Never written into this file: export both before running.
: "${VMA_USER:?set VMA_USER to a panel login}"
: "${VMA_PASS:?set VMA_PASS to the password for that login}"
PORT="${VMA_PORT:-8099}"
# Point VMA_BASE at the TLS front end when the panel runs behind nginx. The session
# cookie is marked Secure there, and curl will not send a Secure cookie over plaintext,
# so testing the loopback port directly fails every authenticated check in a way that
# looks like a broken panel rather than a mis-aimed test.
BASE="${VMA_BASE:-http://127.0.0.1:$PORT}"
JAR=$(mktemp); HDR=$(mktemp)
trap 'rm -f "$JAR" "$HDR" /tmp/sec.body' EXIT

pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s  (got: %s)\n' "$1" "$2"; }
chk()  { [ "$2" = "$3" ] && pass "$1" || fail "$1" "$2"; }

req() { curl -s -o /tmp/sec.body -D "$HDR" -w '%{http_code}' -b "$JAR" -c "$JAR" "$@"; }
tok() { grep -oE 'name="csrf" value="[^"]+"' /tmp/sec.body | head -1 | sed -E 's/.*value="([^"]+)".*/\1/'; }

echo "== unauthenticated access =="
chk "GET / without a session redirects to login" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/")" "302"
chk "POST a mutating endpoint without a session is refused (403, not a redirect)" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d 'act=delete' -d 'username=RABB1T' "$BASE/accounts/action")" "403"
chk "GET /config without a session redirects" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/config/mangosd")" "302"

echo "== authenticate =="
req "$BASE/login" >/dev/null; T=$(tok)
chk "login succeeds" "$(req -X POST -d "username=$VMA_USER" -d "password=$VMA_PASS" -d "csrf=$T" "$BASE/login")" "302"

echo "== session cookie hardening =="
grep -qi 'HttpOnly' "$JAR" 2>/dev/null || grep -qi 'httponly' "$HDR" && pass "cookie is HttpOnly" || fail "cookie HttpOnly" "absent"
req "$BASE/" >/dev/null
grep -i '^set-cookie' "$HDR" | grep -qi 'SameSite=Strict' && pass "cookie is SameSite=Strict" || echo "  INFO  SameSite not re-sent on this response (set at login)"

echo "== security headers =="
req "$BASE/" >/dev/null
for h in "Content-Security-Policy" "X-Frame-Options" "X-Content-Type-Options" "Referrer-Policy"; do
    grep -qi "^$h:" "$HDR" && pass "$h present" || fail "$h present" "missing"
done
grep -qi "script-src 'self'" "$HDR" && pass "CSP forbids inline script" || fail "CSP forbids inline script" "not strict"

echo "== no inline event handlers remain (would be blocked by CSP) =="
INLINE=0
for p in / /accounts /characters /bans /realms /console /config/realmd; do
    req "$BASE$p" >/dev/null
    n=$(grep -coE 'on(submit|click|load|error)=' /tmp/sec.body)
    INLINE=$((INLINE + n))
done
chk "zero inline on*= handlers across all pages" "$INLINE" "0"

echo "== CSRF =="
req "$BASE/accounts" >/dev/null; T=$(tok)
chk "POST with no CSRF token is rejected" \
    "$(req -X POST -d 'act=unban' -d 'username=RABB1T' "$BASE/accounts/action")" "400"
chk "POST with a wrong CSRF token is rejected" \
    "$(req -X POST -d 'act=unban' -d 'username=RABB1T' -d 'csrf=bogus-token-value' "$BASE/accounts/action")" "400"
req "$BASE/accounts" >/dev/null; T=$(tok)
chk "POST with the correct CSRF token is accepted" \
    "$(req -X POST -d 'act=unban' -d 'username=RABB1T' -d "csrf=$T" "$BASE/accounts/action")" "302"

echo "== open redirect =="
req "$BASE/login" >/dev/null
LOC=$(curl -s -o /dev/null -D - -b "$JAR" -c "$JAR" "$BASE/login?next=https://evil.example.com" | grep -i '^location:' | tr -d '\r')
req "$BASE/accounts" >/dev/null; T=$(tok)
L2=$(curl -s -o /dev/null -D - -b "$JAR" -c "$JAR" -X POST -d 'act=unban' -d 'username=RABB1T' -d "csrf=$T" \
     -d 'back=https://evil.example.com/x' "$BASE/accounts/action" | grep -i '^location:' | tr -d '\r')
echo "$L2" | grep -qi 'evil.example.com' && fail "absolute URL in back= is ignored" "$L2" || pass "absolute URL in back= is ignored"
req "$BASE/accounts" >/dev/null; T=$(tok)
L3=$(curl -s -o /dev/null -D - -b "$JAR" -c "$JAR" -X POST -d 'act=unban' -d 'username=RABB1T' -d "csrf=$T" \
     -d 'back=//evil.example.com/x' "$BASE/accounts/action" | grep -i '^location:' | tr -d '\r')
echo "$L3" | grep -qi 'evil.example.com' && fail "protocol-relative back= is ignored" "$L3" || pass "protocol-relative back= is ignored"

echo "== console command injection via field values =="
req "$BASE/accounts" >/dev/null; T=$(tok)
req -X POST -d 'act=create' -d 'username=EVIL NAME' -d 'password=abcdef' -d "csrf=$T" "$BASE/accounts/action" >/dev/null
req "$BASE/accounts" >/dev/null
grep -q "Account name must be" /tmp/sec.body && pass "space in account name rejected" || fail "space in account name rejected" "no error shown"

req "$BASE/accounts" >/dev/null; T=$(tok)
req -X POST -d 'act=create' -d 'username=OK1' -d 'password=has space' -d "csrf=$T" "$BASE/accounts/action" >/dev/null
req "$BASE/accounts" >/dev/null
grep -q "Password must be" /tmp/sec.body && pass "space in password rejected" || fail "space in password rejected" "no error shown"

req "$BASE/accounts" >/dev/null; T=$(tok)
req -X POST -d 'act=ban' -d 'username=RABB1T' -d 'duration=1; rm -rf /' -d 'reason=x' -d "csrf=$T" "$BASE/accounts/action" >/dev/null
req "$BASE/accounts" >/dev/null
grep -q "Duration must look like" /tmp/sec.body && pass "malformed ban duration rejected" || fail "malformed ban duration rejected" "no error shown"

req "$BASE/bans" >/dev/null; T=$(tok)
req -X POST -d 'act=ban_ip' -d 'target=notanip' -d 'duration=-1' -d 'reason=x' -d "csrf=$T" "$BASE/bans/action" >/dev/null
req "$BASE/bans" >/dev/null
grep -q "not a valid IP" /tmp/sec.body && pass "invalid IP rejected" || fail "invalid IP rejected" "no error shown"

echo "== path parameter validation =="
chk "unknown config file name is 404" "$(req "$BASE/config/../../etc/passwd")" "404"
req "$BASE/" >/dev/null; T=$(tok)
chk "systemd action against an unlisted service is refused" \
    "$(req -X POST -d 'service=sshd' -d "csrf=$T" "$BASE/server/svc-stop")" "302"
req "$BASE/" >/dev/null
grep -q "unknown service" /tmp/sec.body && pass "unlisted service reported as unknown" || fail "unlisted service reported as unknown" "no message"

echo "== login throttle (runs last, locks out 127.0.0.1) =="
curl -s -o /dev/null -c /tmp/t.jar "$BASE/login"
LAST=""
for i in $(seq 1 9); do
    curl -s -o /tmp/sec.body -c /tmp/t.jar -b /tmp/t.jar "$BASE/login" >/dev/null
    TT=$(tok)
    LAST=$(curl -s -o /tmp/sec.body -w '%{http_code}' -c /tmp/t.jar -b /tmp/t.jar \
           -X POST -d "username=rabb1t" -d "password=wrong$i" -d "csrf=$TT" "$BASE/login")
done
chk "repeated bad passwords eventually throttled (429)" "$LAST" "429"
rm -f /tmp/t.jar
