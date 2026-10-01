"""vmangos admin panel.

LAN-only web front end for administering the server without touching the database or
config files by hand. Live actions go out over the loopback SOAP channel as GM console
commands; listings come from direct parameterised DB queries.

Security posture, since this process can create accounts and restart the server:
  - Authentication is deny-by-default. Every endpoint requires a session unless it is
    named in PUBLIC_ENDPOINTS, so a route added later is protected even if whoever adds
    it forgets to think about auth.
  - Every state change is POST and carries a CSRF token compared in constant time.
  - Values interpolated into console commands are validated against a charset, because
    commands are single-line and space-delimited (see security.py).
  - Redirect targets are restricted to same-origin relative paths.
  - Failed logins are throttled per source address.

TLS is terminated by nginx, which proxies to this process on loopback. Set
VMA_BEHIND_PROXY=1 for that deployment: it marks the session cookie Secure, emits HSTS,
and tells waitress to honour the X-Forwarded-* headers so the login throttle sees real
client addresses rather than 127.0.0.1 -- without which every client shares one throttle
bucket and eight failures from anywhere lock out the operator. Leave it unset when
running the app directly, because honouring those headers with no proxy in front would
let a client declare its own source address.
"""
import os
import re
import ssl
import time
import socket
import secrets
import datetime
import functools
import subprocess

from flask import (Flask, render_template, request, redirect, url_for, session,
                   flash, abort)
from werkzeug.security import check_password_hash

import db as dbmod
import soap as soapmod
import confmgr
import security as sec

MANGOSD_CONF = os.environ.get("VMA_MANGOSD_CONF", "/home/rabb1t/server/etc/mangosd.conf")
REALMD_CONF = os.environ.get("VMA_REALMD_CONF", "/home/rabb1t/server/etc/realmd.conf")
SOAP_URL = os.environ.get("VMA_SOAP_URL", "http://127.0.0.1:7878/")
SOAP_USER = os.environ.get("VMA_SOAP_USER", "")
SOAP_PASS = os.environ.get("VMA_SOAP_PASSWORD", "")
ADMIN_USER = os.environ.get("VMA_ADMIN_USER", "rabb1t")
ADMIN_HASH = os.environ.get("VMA_ADMIN_HASH", "")
BEHIND_PROXY = os.environ.get("VMA_BEHIND_PROXY", "") == "1"
# Hostname on the certificate. Empty disables the expiry panel on the dashboard.
CERT_HOST = os.environ.get("VMA_CERT_HOST", "")
# Renewal of this certificate is manual, so the warning has to land well before expiry.
CERT_WARN_DAYS = int(os.environ.get("VMA_CERT_WARN_DAYS", "25"))

SERVICES = {"world": "vmangos-mangosd", "realm": "vmangos-realmd"}
CONF_PATHS = {"mangosd": MANGOSD_CONF, "realmd": REALMD_CONF}

# Endpoints reachable without a session. Everything else is gated by require_login().
PUBLIC_ENDPOINTS = {"login", "static"}

SEC_LEVELS = {
    0: "Player", 1: "Moderator", 2: "Ticket Master", 3: "Game Master",
    4: "Basic Admin", 5: "Developer", 6: "Administrator", 7: "Console",
}
RACES = {1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf",
         5: "Undead", 6: "Tauren", 7: "Gnome", 8: "Troll"}
CLASSES = {1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
           7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid"}

CSP = ("default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
       "img-src 'self' data:; form-action 'self'; frame-ancestors 'none'; "
       "base-uri 'none'; object-src 'none'")

app = Flask(__name__)
app.secret_key = os.environ.get("VMA_SECRET") or secrets.token_hex(32)
app.config.update(
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Strict",
    SESSION_COOKIE_NAME="vmangos_admin",
    PERMANENT_SESSION_LIFETIME=8 * 3600,
    MAX_CONTENT_LENGTH=2 * 1024 * 1024,
    SESSION_COOKIE_SECURE=BEHIND_PROXY,
)

DB = dbmod.DB(MANGOSD_CONF)
THROTTLE = sec.LoginThrottle()


# ---------------------------------------------------------------- plumbing

def soap(command):
    return soapmod.execute(SOAP_URL, SOAP_USER, SOAP_PASS, command)


_cert_cache = {"at": 0.0, "value": None}


def cert_status():
    """How long the served certificate has left, or None if not configured.

    Read from the TLS front end over a real connection rather than from the file on
    disk. That needs no privilege, since this process cannot read /etc/letsencrypt, and
    it reports what a browser is actually offered -- so a renewal that never reached
    nginx shows up here instead of looking fine until the old certificate expires.

    This certificate is renewed by hand, which is the whole reason for surfacing it.
    """
    if not CERT_HOST:
        return None

    now = time.time()
    if _cert_cache["value"] is not None and now - _cert_cache["at"] < 900:
        return _cert_cache["value"]

    try:
        ctx = ssl.create_default_context()
        with socket.create_connection((CERT_HOST, 443), timeout=4) as raw:
            with ctx.wrap_socket(raw, server_hostname=CERT_HOST) as tls:
                not_after = tls.getpeercert()["notAfter"]
        expires = datetime.datetime.strptime(not_after, "%b %d %H:%M:%S %Y %Z")
        days = (expires - datetime.datetime.utcnow()).days
        value = {"expires": expires.strftime("%Y-%m-%d"), "days": days,
                 "warn": days <= CERT_WARN_DAYS, "error": None}
    except Exception as e:
        value = {"expires": None, "days": None, "warn": True, "error": str(e)}

    _cert_cache.update(at=now, value=value)
    return value


def run_soap_flash(command, success_note=None):
    ok, out = soap(command)
    out = (out or "").strip()
    if ok:
        flash(success_note or (out or "Command completed."), "ok")
    else:
        flash(out or "Command failed.", "err")
    return ok


def csrf_token():
    if "csrf" not in session:
        session["csrf"] = secrets.token_urlsafe(32)
    return session["csrf"]


@app.before_request
def require_login():
    """Deny by default: no session, no access, whatever the endpoint."""
    if request.endpoint in PUBLIC_ENDPOINTS:
        return None
    if request.endpoint is None:
        return None  # 404 handling, nothing to protect
    if not session.get("user"):
        if request.method != "GET":
            abort(403)
        return redirect(url_for("login", next=request.full_path.rstrip("?")))
    return None


@app.before_request
def verify_csrf():
    if request.method in ("POST", "PUT", "PATCH", "DELETE"):
        sent = request.form.get("csrf", "")
        held = session.get("csrf", "")
        if not sent or not held or not secrets.compare_digest(str(sent), str(held)):
            abort(400, "CSRF token missing or stale. Reload the page and try again.")


@app.after_request
def security_headers(resp):
    resp.headers["Content-Security-Policy"] = CSP
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["X-Frame-Options"] = "DENY"
    resp.headers["Referrer-Policy"] = "no-referrer"
    resp.headers["Cross-Origin-Opener-Policy"] = "same-origin"
    resp.headers["Cache-Control"] = "no-store"
    resp.headers["Permissions-Policy"] = "geolocation=(), microphone=(), camera=()"
    if BEHIND_PROXY:
        # No includeSubDomains: this host's name sits under a zone whose other names are
        # not ours to make promises about.
        resp.headers["Strict-Transport-Security"] = "max-age=31536000"
    return resp


@app.context_processor
def inject():
    return {"csrf_token": csrf_token, "sec_levels": SEC_LEVELS,
            "current_user": session.get("user")}


def back_to(default_endpoint):
    return sec.safe_next(request.form.get("back"), url_for(default_endpoint))


def systemctl(action, service):
    if service not in SERVICES.values():
        return False, "unknown service"
    if action not in ("start", "stop", "restart"):
        return False, "unknown action"
    try:
        p = subprocess.run(["sudo", "-n", "systemctl", action, service],
                           capture_output=True, text=True, timeout=180)
        out = (p.stdout + p.stderr).strip()
        return p.returncode == 0, out or "%s %s ok" % (action, service)
    except subprocess.SubprocessError as e:
        return False, str(e)


def service_state(service):
    try:
        p = subprocess.run(["systemctl", "is-active", service],
                           capture_output=True, text=True, timeout=10)
        return p.stdout.strip() or "unknown"
    except subprocess.SubprocessError:
        return "unknown"


def safe(read_fn, default=None, label="data"):
    """DB reads must never take the whole page down.

    A failure is surfaced in the UI as well as the log: an empty table that silently
    means "the query broke" is indistinguishable from one that means "there is nothing
    here", and that costs more time than it saves.
    """
    try:
        return read_fn()
    except Exception as e:  # noqa: BLE001 - surfaced in the UI instead
        app.logger.warning("db read failed (%s): %s", label, e)
        flash("Could not read %s: %s" % (label, e), "warn")
        return default if default is not None else []


def reject(message, endpoint):
    flash(message, "err")
    return redirect(url_for(endpoint))


# ---------------------------------------------------------------- auth

@app.route("/login", methods=["GET", "POST"])
def login():
    if session.get("user"):
        return redirect(url_for("dashboard"))

    if request.method == "POST":
        source = request.remote_addr or "unknown"
        wait = THROTTLE.locked_for(source)
        if wait:
            flash("Too many failed attempts. Try again in %d seconds." % wait, "err")
            return render_template("login.html"), 429

        user = request.form.get("username", "")
        password = request.form.get("password", "")
        # Always run the hash comparison so a wrong username and a wrong password cost
        # the same amount of time.
        user_ok = secrets.compare_digest(user, ADMIN_USER)
        pass_ok = bool(ADMIN_HASH) and check_password_hash(ADMIN_HASH, password)

        if user_ok and pass_ok:
            THROTTLE.reset(source)
            session.clear()
            session["user"] = ADMIN_USER
            session.permanent = True
            csrf_token()
            app.logger.info("admin login from %s", source)
            return redirect(sec.safe_next(request.args.get("next"), url_for("dashboard")))

        THROTTLE.record_failure(source)
        app.logger.warning("failed admin login from %s", source)
        flash("Invalid credentials.", "err")

    return render_template("login.html")


@app.route("/logout", methods=["POST"])
def logout():
    session.clear()
    flash("Signed out.", "ok")
    return redirect(url_for("login"))


# ---------------------------------------------------------------- dashboard

@app.route("/")
def dashboard():
    ok, info = soap("server info")
    online_ok, online = soap("account onlinelist")

    accounts = safe(lambda: DB.query("login", "SELECT COUNT(*) AS n FROM account")[0]["n"],
                    0, "account count")
    chars = safe(lambda: DB.query("char", "SELECT COUNT(*) AS n FROM characters")[0]["n"],
                 0, "character count")
    online_chars = safe(lambda: DB.query(
        "char", "SELECT `name`,`level`,`race`,`class` FROM characters "
                "WHERE online=1 ORDER BY `name`"), [], "online characters")
    realms = safe(lambda: DB.query(
        "login", "SELECT id,name,address,localAddress,port FROM realmlist ORDER BY id"),
        [], "realms")

    return render_template(
        "dashboard.html",
        server_info=info if ok else "SOAP unreachable: %s" % info,
        server_ok=ok,
        onlinelist=online if online_ok else "",
        accounts=accounts, chars=chars, online_chars=online_chars,
        races=RACES, classes=CLASSES, realms=realms,
        states={k: service_state(v) for k, v in SERVICES.items()},
        services=SERVICES,
        cert=cert_status(),
    )


@app.route("/server/<action>", methods=["POST"])
def server_action(action):
    if action == "saveall":
        run_soap_flash("saveall", "All players saved.")

    elif action in ("announce", "notify"):
        msg = sec.clean_text(request.form.get("message"))
        if not msg:
            flash("Nothing to send.", "warn")
        else:
            run_soap_flash("%s %s" % (action, msg), "%s sent: %s" % (action.title(), msg))

    elif action == "motd":
        msg = sec.clean_text(request.form.get("message"))
        run_soap_flash("server set motd %s" % msg, "MOTD updated.")

    elif action == "plimit":
        val = (request.form.get("value") or "").strip()
        if not re.fullmatch(r"-?\d{1,6}", val):
            flash("Player limit must be a whole number.", "err")
        else:
            run_soap_flash("server plimit %s" % val,
                           "Player limit set to %s (live, no restart)." % val)

    elif action in ("shutdown", "restart"):
        delay = (request.form.get("delay") or "60").strip()
        if not re.fullmatch(r"\d{1,6}", delay):
            flash("Delay must be a whole number of seconds.", "err")
        else:
            run_soap_flash("server %s %s" % (action, delay),
                           "World %s scheduled in %ss." % (action, delay))

    elif action in ("svc-restart", "svc-stop", "svc-start"):
        verb = {"svc-restart": "restart", "svc-stop": "stop", "svc-start": "start"}[action]
        ok, out = systemctl(verb, request.form.get("service", ""))
        flash(out, "ok" if ok else "err")

    else:
        abort(404)

    return redirect(back_to("dashboard"))


# ---------------------------------------------------------------- accounts

@app.route("/accounts")
def accounts():
    q = (request.args.get("q") or "").strip()
    # Security lives in account_access per realm; account.gmlevel is the legacy single
    # value. Take whichever is higher so the UI never understates someone's access.
    sql = ("SELECT a.id, a.username, a.email, a.last_ip, a.locked, a.online, "
           "a.joindate, a.last_login, "
           "GREATEST(COALESCE(a.gmlevel, 0), COALESCE("
           "(SELECT MAX(aa.gmlevel) FROM account_access aa WHERE aa.id = a.id), 0)"
           ") AS gmlevel "
           "FROM account a ")
    args = ()
    if q:
        sql += "WHERE a.username LIKE %s "
        args = ("%" + q + "%",)
    sql += "ORDER BY a.id"

    rows = safe(lambda: DB.query("login", sql, args), [], "accounts")
    banned = safe(lambda: {r["id"] for r in DB.query(
        "login", "SELECT id FROM account_banned WHERE active=1")}, set(), "account bans")
    return render_template("accounts.html", accounts=rows, q=q, banned=banned)


@app.route("/accounts/action", methods=["POST"])
def accounts_action():
    act = request.form.get("act", "")
    user = (request.form.get("username") or "").strip()

    if not sec.valid_name(user):
        return reject("Account name must be 1-32 characters of letters, digits, "
                      "dot, dash or underscore.", "accounts")

    # The panel reaches the server by authenticating to SOAP as this account, so changing
    # or removing it from the panel would cut the branch the panel is sitting on: every
    # later action would fail with an opaque 401.
    if act in ("password", "delete", "gmlevel", "ban") and user.upper() == SOAP_USER.upper():
        return reject(
            "%s is the panel's own SOAP service account. Changing it here would lock the "
            "panel out of the server. Edit admin.env and restart vmangos-admin instead."
            % user, "accounts")

    if act in ("create", "password"):
        pw = request.form.get("password") or ""
        if not sec.valid_password(pw):
            return reject("Password must be 6-%d printable characters with no spaces. "
                          "The server caps account passwords at %d."
                          % (sec.MAX_PASSWORD_LEN, sec.MAX_PASSWORD_LEN), "accounts")
        if act == "create":
            run_soap_flash("account create %s %s" % (user, pw),
                           "Account %s created at Player level." % user)
        else:
            run_soap_flash("account set password %s %s %s" % (user, pw, pw),
                           "Password updated for %s." % user)

    elif act == "gmlevel":
        lvl = (request.form.get("gmlevel") or "").strip()
        realm = (request.form.get("realm") or "-1").strip()
        if not re.fullmatch(r"[0-7]", lvl) or not re.fullmatch(r"-?\d{1,6}", realm):
            return reject("Invalid security level or realm id.", "accounts")
        run_soap_flash("account set gmlevel %s %s %s" % (user, lvl, realm),
                       "%s set to %s on realm %s." % (user, SEC_LEVELS[int(lvl)], realm))

    elif act == "locked":
        onoff = "on" if request.form.get("locked") == "1" else "off"
        run_soap_flash("account set locked %s %s" % (user, onoff),
                       "IP lock %s for %s." % (onoff, user))

    elif act == "delete":
        run_soap_flash("account delete %s" % user, "Account %s deleted." % user)

    elif act == "ban":
        dur = (request.form.get("duration") or "-1").strip()
        if not sec.valid_duration(dur):
            return reject("Duration must look like -1, 600, 10m, 2h or 7d.", "accounts")
        reason = sec.clean_text(request.form.get("reason")) or "banned via admin panel"
        run_soap_flash("ban account %s %s %s" % (user, dur, reason),
                       "Banned %s (%s)." % (user, dur))

    elif act == "unban":
        run_soap_flash("unban account %s" % user, "Unbanned %s." % user)

    else:
        abort(404)

    return redirect(back_to("accounts"))


# ---------------------------------------------------------------- characters

@app.route("/characters")
def characters():
    q = (request.args.get("q") or "").strip()
    sql = ("SELECT c.guid, c.account, c.name, c.race, c.`class` AS class, c.gender, "
           "c.level, c.money, c.online, c.map FROM characters c ")
    args = ()
    if q:
        sql += "WHERE c.name LIKE %s "
        args = ("%" + q + "%",)
    sql += "ORDER BY c.online DESC, c.level DESC, c.name LIMIT 500"

    rows = safe(lambda: DB.query("char", sql, args), [], "characters")
    names = {}
    if rows:
        ids = sorted({r["account"] for r in rows})
        placeholders = ",".join(["%s"] * len(ids))
        names = safe(lambda: {r["id"]: r["username"] for r in DB.query(
            "login", "SELECT id, username FROM account WHERE id IN (%s)" % placeholders,
            tuple(ids))}, {}, "account names")
    return render_template("characters.html", chars=rows, q=q, acct_names=names,
                           races=RACES, classes=CLASSES)


@app.route("/characters/action", methods=["POST"])
def characters_action():
    act = request.form.get("act", "")
    name = (request.form.get("name") or "").strip()
    if not sec.valid_name(name):
        return reject("Invalid character name.", "characters")

    if act == "kick":
        run_soap_flash("kick %s" % name, "Kicked %s." % name)
    elif act == "ban":
        dur = (request.form.get("duration") or "-1").strip()
        if not sec.valid_duration(dur):
            return reject("Duration must look like -1, 600, 10m, 2h or 7d.", "characters")
        reason = sec.clean_text(request.form.get("reason")) or "banned via admin panel"
        run_soap_flash("ban character %s %s %s" % (name, dur, reason),
                       "Banned character %s." % name)
    elif act == "unban":
        run_soap_flash("unban character %s" % name, "Unbanned character %s." % name)
    else:
        abort(404)

    return redirect(back_to("characters"))


# ---------------------------------------------------------------- bans

@app.route("/bans")
def bans():
    acct = safe(lambda: DB.query("login",
        "SELECT b.id, a.username, b.bandate, b.unbandate, b.bannedby, b.banreason, "
        "b.active FROM account_banned b LEFT JOIN account a ON a.id=b.id "
        "ORDER BY b.bandate DESC LIMIT 200"), [], "account bans")
    ips = safe(lambda: DB.query("login",
        "SELECT ip, bandate, unbandate, bannedby, banreason FROM ip_banned "
        "ORDER BY bandate DESC LIMIT 200"), [], "IP bans")
    chars = safe(lambda: DB.query("char",
        "SELECT b.guid, c.name, b.bandate, b.unbandate, b.bannedby, b.banreason, "
        "b.active FROM character_banned b LEFT JOIN characters c ON c.guid=b.guid "
        "ORDER BY b.bandate DESC LIMIT 200"), [], "character bans")
    return render_template("bans.html", acct=acct, ips=ips, chars=chars)


@app.route("/bans/action", methods=["POST"])
def bans_action():
    act = request.form.get("act", "")
    target = (request.form.get("target") or "").strip()

    if act in ("ban_ip", "unban_ip"):
        if not sec.valid_ip(target):
            return reject("That is not a valid IP address.", "bans")
    elif not sec.valid_name(target):
        return reject("Invalid target name.", "bans")

    if act == "ban_ip":
        dur = (request.form.get("duration") or "-1").strip()
        if not sec.valid_duration(dur):
            return reject("Duration must look like -1, 600, 10m, 2h or 7d.", "bans")
        reason = sec.clean_text(request.form.get("reason")) or "banned via admin panel"
        run_soap_flash("ban ip %s %s %s" % (target, dur, reason), "Banned IP %s." % target)
    elif act == "unban_ip":
        run_soap_flash("unban ip %s" % target, "Unbanned IP %s." % target)
    elif act == "unban_account":
        run_soap_flash("unban account %s" % target, "Unbanned account %s." % target)
    elif act == "unban_character":
        run_soap_flash("unban character %s" % target, "Unbanned character %s." % target)
    else:
        abort(404)

    return redirect(url_for("bans"))


# ---------------------------------------------------------------- realms

@app.route("/realms")
def realms():
    rows = safe(lambda: DB.query("login", "SELECT * FROM realmlist ORDER BY id"),
                [], "realms")
    return render_template("realms.html", realms=rows)


@app.route("/realms/save", methods=["POST"])
def realms_save():
    rid = (request.form.get("id") or "").strip()
    if not re.fullmatch(r"\d{1,6}", rid):
        abort(400)

    name = sec.clean_text(request.form.get("name"), 32)
    address = (request.form.get("address") or "").strip()
    local_address = (request.form.get("localAddress") or "").strip()
    mask = (request.form.get("localSubnetMask") or "").strip()
    port = (request.form.get("port") or "").strip()
    flags = (request.form.get("realmflags") or "0").strip()
    minsec = (request.form.get("allowedSecurityLevel") or "0").strip()

    if not name or not address:
        return reject("Realm name and address are required.", "realms")
    if not re.fullmatch(r"\d{1,5}", port) or not 1 <= int(port) <= 65535:
        return reject("Port must be between 1 and 65535.", "realms")
    if not re.fullmatch(r"\d{1,3}", flags) or not re.fullmatch(r"[0-7]", minsec):
        return reject("Invalid realm flags or minimum security level.", "realms")
    # address may be a hostname; localAddress and the mask must be literal IPv4, since
    # realmd parses the mask numerically and compares it against the client address.
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,255}", address):
        return reject("Address must be a hostname or IPv4 literal.", "realms")
    if local_address and not sec.valid_ip(local_address):
        return reject("Local address must be an IPv4 literal.", "realms")
    if mask and not sec.valid_ip(mask):
        return reject("Local subnet mask must look like 255.255.255.0.", "realms")

    try:
        DB.exec("login",
                "UPDATE realmlist SET name=%s, address=%s, localAddress=%s, "
                "localSubnetMask=%s, port=%s, realmflags=%s, allowedSecurityLevel=%s "
                "WHERE id=%s",
                (name, address, local_address, mask, port, flags, minsec, rid))
    except Exception as e:  # noqa: BLE001
        return reject("Update failed: %s" % e, "realms")

    flash("Realm %s saved. realmd re-reads the realm list every 20s, so this applies "
          "shortly with no restart. A hostname that does not resolve causes realmd to "
          "drop the realm from the list, so confirm DNS first." % rid, "ok")
    return redirect(url_for("realms"))


# ---------------------------------------------------------------- config files

@app.route("/config/<which>")
def config_view(which):
    if which not in CONF_PATHS:
        abort(404)
    q = (request.args.get("q") or "").strip().lower()
    groups = confmgr.grouped(CONF_PATHS[which])
    if q:
        groups = {g: [e for e in es if q in e["key"].lower() or q in e["value"].lower()]
                  for g, es in groups.items()}
        groups = {g: es for g, es in groups.items() if es}
    return render_template("config.html", which=which, groups=groups, q=q,
                           path=CONF_PATHS[which], conf_paths=CONF_PATHS)


@app.route("/config/<which>/save", methods=["POST"])
def config_save(which):
    if which not in CONF_PATHS:
        abort(404)
    changes = {}
    for key, value in request.form.items():
        if key.startswith("cfg:"):
            changes[key[4:]] = sec.clean_config_value(value)
    if not changes:
        flash("Nothing submitted.", "warn")
        return redirect(url_for("config_view", which=which))
    try:
        bak, n = confmgr.save(CONF_PATHS[which], changes)
    except Exception as e:  # noqa: BLE001
        return reject("Save failed: %s" % e, "config_view")
    if n:
        flash("%d setting(s) written. Backup: %s. Most settings are only read at "
              "startup, so restart the service to apply them."
              % (n, os.path.basename(bak)), "ok")
    else:
        flash("No values differed, nothing written.", "warn")
    return redirect(url_for("config_view", which=which,
                            q=sec.clean_text(request.form.get("q"), 64)))


# ---------------------------------------------------------------- console

@app.route("/console", methods=["GET", "POST"])
def console():
    output = None
    command = ""
    if request.method == "POST":
        # Deliberately unrestricted: this is the escape hatch for the hundreds of
        # commands without a dedicated page. It is gated by the session, and newlines
        # are stripped so one submission cannot become several commands.
        command = sec.clean_text(request.form.get("command"), 300).lstrip(".")
        if command:
            ok, out = soap(command)
            output = out or ("(no output)" if ok else "(failed, no output)")
            if not ok:
                flash("Command reported an error.", "err")
    return render_template("console.html", output=output, command=command)


# ---------------------------------------------------------------- errors

@app.errorhandler(400)
def err_400(e):
    return render_template("error.html", code=400, title="Bad request",
                           detail=getattr(e, "description", "")), 400


@app.errorhandler(403)
def err_403(e):
    return render_template("error.html", code=403, title="Forbidden",
                           detail="Your session is missing or expired."), 403


@app.errorhandler(404)
def err_404(e):
    return render_template("error.html", code=404, title="Not found",
                           detail="No such page."), 404


@app.errorhandler(500)
def err_500(e):
    app.logger.exception("unhandled error")
    return render_template("error.html", code=500, title="Server error",
                           detail="Check the service journal for details."), 500


if __name__ == "__main__":
    if not ADMIN_HASH:
        raise SystemExit("VMA_ADMIN_HASH is not set; refusing to start with no password.")
    from waitress import serve

    proxy = {}
    if BEHIND_PROXY:
        # waitress defaults to clear_untrusted_proxy_headers=True with no trusted_proxy,
        # so it strips X-Forwarded-* before the application ever sees them. Naming the
        # proxy here is what makes them visible, and it is a stronger check than fixing
        # up the headers in WSGI middleware would be: waitress only honours them when
        # the peer really is the local nginx, rather than trusting whoever connected.
        proxy = dict(
            trusted_proxy="127.0.0.1",
            trusted_proxy_count=1,
            trusted_proxy_headers={"x-forwarded-for", "x-forwarded-proto",
                                   "x-forwarded-host"},
        )

    serve(app, host=os.environ.get("VMA_BIND", "0.0.0.0"),
          port=int(os.environ.get("VMA_PORT", "8099")),
          threads=8, ident="vmangos-admin", **proxy)
