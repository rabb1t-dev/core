#!/bin/bash
# Exercise every page of the admin panel and report HTTP status + any server-side error.
export HISTFILE=/dev/null
# The panel login to test with. Never written into this file: export both before running.
: "${VMA_USER:?set VMA_USER to a panel login}"
: "${VMA_PASS:?set VMA_PASS to the password for that login}"
PORT="${VMA_PORT:-8099}"
# See sectest.sh: set VMA_BASE to the https front end when the panel sits behind nginx,
# because the Secure session cookie is not sent over plaintext.
BASE="${VMA_BASE:-http://127.0.0.1:$PORT}"
JAR=$(mktemp)
trap 'rm -f "$JAR"' EXIT

code() { curl -s -o /tmp/smoke.body -w '%{http_code}' -b "$JAR" -c "$JAR" "$@"; }

echo "-- login page --"
printf '  GET  /login            %s\n' "$(code "$BASE/login")"
TOKEN=$(grep -oE 'name="csrf" value="[^"]+"' /tmp/smoke.body | head -1 | sed -E 's/.*value="([^"]+)".*/\1/')
[ -n "$TOKEN" ] && echo "  csrf token obtained" || echo "  NO CSRF TOKEN FOUND"

echo "-- authenticate --"
printf '  POST /login            %s\n' \
  "$(code -X POST -d "username=$VMA_USER" -d "password=$VMA_PASS" -d "csrf=$TOKEN" "$BASE/login")"

echo "-- authenticated pages --"
for p in / /accounts /characters /bans /realms /console /config/mangosd /config/realmd; do
    C=$(code "$BASE$p")
    SIZE=$(wc -c < /tmp/smoke.body | tr -d ' ')
    printf '  GET  %-21s %s  (%s bytes)\n' "$p" "$C" "$SIZE"
    if [ "$C" != "200" ]; then
        echo "    ---- body head ----"
        head -c 600 /tmp/smoke.body | sed 's/^/    /'
    fi
done

echo "-- a read-only console command through the UI --"
C=$(code "$BASE/console")
TOKEN=$(grep -oE 'name="csrf" value="[^"]+"' /tmp/smoke.body | head -1 | sed -E 's/.*value="([^"]+)".*/\1/')
printf '  POST /console          %s\n' \
  "$(code -X POST -d "command=server info" -d "csrf=$TOKEN" "$BASE/console")"
grep -oE "Core revision[^<]*" /tmp/smoke.body | head -1 | sed 's/^/    /'

echo "-- unauthenticated access is refused --"
printf '  GET  / (no cookie)     %s (expect 302 to login)\n' \
  "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/")"

echo "-- server-side errors in the journal --"
sudo journalctl -u vmangos-admin --no-pager -n 200 2>/dev/null \
  | grep -iE "traceback|error|exception" | tail -10 || echo "  none"
