"""MaNGOS SOAP client: runs a GM console command over the loopback SOAP channel.

Every live administrative action (account create/password/gmlevel, bans, kicks,
announcements, saveall, shutdown) is a console command, so this one function is the
whole live-admin surface.
"""
import html
import requests
from xml.etree import ElementTree as ET

_ENVELOPE = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/"'
    ' xmlns:ns1="urn:MaNGOS"><SOAP-ENV:Body>'
    "<ns1:executeCommand><command>{cmd}</command></ns1:executeCommand>"
    "</SOAP-ENV:Body></SOAP-ENV:Envelope>"
)


def execute(url, user, password, command, timeout=20):
    """Returns (ok: bool, text: str). text is the command output or the fault message."""
    body = _ENVELOPE.format(cmd=html.escape(command))
    try:
        r = requests.post(
            url, data=body.encode("utf-8"),
            headers={"Content-Type": "text/xml"},
            auth=(user, password), timeout=timeout,
        )
    except requests.RequestException as e:
        return False, "SOAP request failed: %s" % e

    # The auth gate in MaNGOSsoap.cpp answers with a bare HTTP status and no SOAP envelope,
    # so naming these two keeps them from surfacing as an opaque "HTTP 401 Unauthorized".
    if r.status_code == 401:
        return False, (
            "SOAP rejected the panel's credentials. VMA_SOAP_USER/VMA_SOAP_PASSWORD in "
            "admin.env no longer match a server account -- most likely that account's "
            "password was changed."
        )
    if r.status_code == 403:
        return False, (
            "SOAP account '%s' is below Administrator level. It needs gmlevel 6 in "
            "account_access, and the world must be restarted to load it." % user
        )

    text = r.text
    try:
        root = ET.fromstring(text)
        result = None
        fault = None
        for el in root.iter():
            tag = el.tag.split("}")[-1]
            if tag == "result" and el.text is not None:
                result = el.text
            elif tag == "faultstring" and el.text is not None:
                fault = el.text
        if fault is not None:
            return False, fault.strip()
        if result is not None:
            return True, result.strip()
    except ET.ParseError:
        pass
    return (r.status_code == 200), text.strip()
