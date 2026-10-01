"""Input validation, redirect safety and login throttling for the admin panel.

Two threats shape this module:

1. Console command injection. Every live action is a single-line, space-delimited GM
   command sent over SOAP. A space in an account name, or a newline in a ban reason,
   would silently become extra arguments or an extra command. So values destined for a
   command are validated against a charset rather than merely escaped.

2. Credential guessing against the login form. The panel is LAN-only, but "LAN-only"
   includes anything that gets a foothold on the LAN, so failed logins are throttled.
"""
import re
import time
import ipaddress
import threading
from urllib.parse import urlparse

# WoW account and character names are alphanumeric in practice. The point of the charset
# is that nothing here can introduce whitespace, a newline, or a quote into a command.
_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,32}$")
_DURATION = re.compile(r"^-?\d{1,10}[smhdwMy]?$")
_CONTROL = re.compile(r"[\x00-\x1f\x7f]")


def valid_name(value):
    return bool(value) and bool(_NAME.match(value))


def valid_password(value):
    if not value or len(value) < 6 or len(value) > 64:
        return False
    # Printable ASCII with no whitespace: the console splits arguments on whitespace.
    return all(33 <= ord(c) <= 126 for c in value)


def valid_duration(value):
    return bool(value) and bool(_DURATION.match(value))


def valid_ip(value):
    try:
        ipaddress.ip_address(value)
        return True
    except ValueError:
        return False


def clean_text(value, maxlen=200):
    """Free text destined for a command tail: announcements, ban reasons, MOTD.

    Control characters are removed outright (a newline is what would let a second
    command ride along) and runs of whitespace collapse to single spaces.
    """
    if not value:
        return ""
    value = _CONTROL.sub(" ", value)
    return re.sub(r"\s+", " ", value).strip()[:maxlen]


def clean_config_value(value):
    """Config values are written verbatim into a line of the file, so a newline here
    would forge additional settings. Keep it to one line."""
    return _CONTROL.sub("", value or "").strip()


def safe_next(target, fallback):
    """Only allow same-origin relative redirects, to close off open redirects."""
    if not target:
        return fallback
    parsed = urlparse(target)
    if parsed.scheme or parsed.netloc:
        return fallback
    if not target.startswith("/") or target.startswith("//"):
        return fallback
    return target


class LoginThrottle:
    """Sliding-window lockout on failed logins, keyed by source address."""

    def __init__(self, max_failures=8, window=300, lockout=300):
        self.max_failures = max_failures
        self.window = window
        self.lockout = lockout
        self._fails = {}
        self._lock = threading.Lock()

    def _prune(self, key, now):
        recent = [t for t in self._fails.get(key, []) if now - t < self.window]
        if recent:
            self._fails[key] = recent
        else:
            self._fails.pop(key, None)
        return recent

    def locked_for(self, key):
        """Seconds remaining in the lockout, or 0 if not locked."""
        now = time.time()
        with self._lock:
            recent = self._prune(key, now)
            if len(recent) >= self.max_failures:
                return int(self.lockout - (now - max(recent))) or 1
            return 0

    def record_failure(self, key):
        with self._lock:
            self._fails.setdefault(key, []).append(time.time())

    def reset(self, key):
        with self._lock:
            self._fails.pop(key, None)
