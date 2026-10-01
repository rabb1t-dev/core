#!/bin/bash
# Install or update the vmangos admin panel on this host.
# Idempotent: safe to re-run after editing the app.
export HISTFILE=/dev/null
set -eu

APP=/home/rabb1t/vmangos-admin
# The app listens on loopback only; nginx terminates TLS on 443 and proxies to it, so
# this port is never what a client connects to and is not opened in the firewall.
PORT="${VMA_PORT:-8099}"
ADMIN_USER="${VMA_ADMIN_USER:-rabb1t}"
ADMIN_PASS="${VMA_ADMIN_PASS:-}"
CERT_HOST="${VMA_CERT_HOST:-admin.moonlight.cx}"

if [ ! -d "$APP" ]; then
    echo "error: $APP does not exist, copy the app there first" >&2
    exit 1
fi
cd "$APP"

if [ ! -x venv/bin/python ]; then
    echo "creating venv"
    python3 -m venv venv
fi
./venv/bin/pip install --quiet --upgrade pip
./venv/bin/pip install --quiet -r requirements.txt
echo "python deps installed"

# admin.env holds the session secret and the admin password hash. Generated once and
# preserved on re-runs so sessions and the password survive an update.
if [ ! -f admin.env ]; then
    if [ -z "$ADMIN_PASS" ]; then
        echo "error: VMA_ADMIN_PASS must be set on first install" >&2
        exit 1
    fi
    SOAP_USER=""; SOAP_PASS=""
    if [ -f /home/rabb1t/.vmangos-harness.env ]; then
        . /home/rabb1t/.vmangos-harness.env
        SOAP_USER="${VMANGOS_SOAP_USER:-}"
        SOAP_PASS="${VMANGOS_SOAP_PASSWORD:-}"
    fi
    HASH=$(P="$ADMIN_PASS" ./venv/bin/python -c "import os; from werkzeug.security import generate_password_hash as g; print(g(os.environ['P']))")
    SECRET=$(./venv/bin/python -c "import secrets; print(secrets.token_hex(32))")
    umask 077
    cat > admin.env <<EOF
VMA_ADMIN_USER=$ADMIN_USER
VMA_ADMIN_HASH=$HASH
VMA_SECRET=$SECRET
VMA_SOAP_URL=http://127.0.0.1:7878/
VMA_SOAP_USER=$SOAP_USER
VMA_SOAP_PASSWORD=$SOAP_PASS
VMA_MANGOSD_CONF=/home/rabb1t/server/etc/mangosd.conf
VMA_REALMD_CONF=/home/rabb1t/server/etc/realmd.conf
VMA_BIND=127.0.0.1
VMA_PORT=$PORT
VMA_BEHIND_PROXY=1
VMA_CERT_HOST=$CERT_HOST
EOF
    chmod 600 admin.env
    echo "admin.env created (mode 600)"
else
    echo "admin.env already present, left alone"
fi

sudo cp vmangos-admin.service /etc/systemd/system/vmangos-admin.service
sudo systemctl daemon-reload
sudo systemctl enable vmangos-admin >/dev/null 2>&1 || true
sudo systemctl restart vmangos-admin
echo "service restarted"

sudo cp nginx-vmangos-admin.conf /etc/nginx/sites-available/vmangos-admin
sudo ln -sf /etc/nginx/sites-available/vmangos-admin /etc/nginx/sites-enabled/vmangos-admin
# Debian ships a default site on port 80, and there is meant to be no plaintext listener.
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl reload nginx || sudo systemctl restart nginx
echo "nginx front end installed"

# Firewall: 443 reachable from RFC1918 space only, never from the internet. Matches the
# treatment SSH already gets in /etc/nftables.conf. The app's own port stays closed,
# since it is bound to loopback.
NFT=/etc/nftables.conf
if ! grep -q 'dport 443 accept comment "admin panel' "$NFT"; then
    sudo cp "$NFT" "$NFT.bak-$(date +%Y%m%d-%H%M%S)"
    sudo python3 - "$NFT" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
anchor = '        ip saddr @private_v4 tcp dport 22 accept comment "ssh (LAN only)"\n'
assert s.count(anchor) == 1, "ssh anchor rule not found, refusing to edit firewall"
new = anchor + '        ip saddr @private_v4 tcp dport 443 accept comment "admin panel https (LAN only)"\n'
open(path, "w").write(s.replace(anchor, new))
print("firewall rule added for port 443")
PY
    # Applied rule by rule rather than with "nft -f", which flushes the table and would
    # drop the ssh session running this script.
    sudo nft -c -f "$NFT"
    sudo nft insert rule inet filter input ip saddr @private_v4 tcp dport 443 accept comment '"admin panel https (LAN only)"'
    echo "firewall rule applied"
else
    echo "firewall rule already present"
fi

sleep 2
systemctl is-active vmangos-admin | sed 's/^/service: /'
ss -ltn | grep "127.0.0.1:$PORT" >/dev/null && echo "app listening on loopback:$PORT" \
    || echo "WARNING: app not listening on 127.0.0.1:$PORT"
ss -ltn | grep ":443" >/dev/null && echo "nginx listening on 443" \
    || echo "WARNING: nginx not listening on 443"
