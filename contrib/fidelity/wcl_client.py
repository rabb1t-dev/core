"""Warcraft Logs v2 client, for auditing our boss scripts against real kills.

The question this exists to answer is not "what do good players do" but "what does the
boss actually do". A Classic Era kill log enumerates every spell the encounter cast, when
it cast it and at whom, which is the one thing no amount of reading our own scripts can
establish. Several of ours say outright that their numbers are invented -- Anub'Rekhan's
cast timer is documented as "best guess so far is random between 12 and 18 seconds", and
Viscidus as "everything is an approximated here" -- and this is how those get replaced
with measurements.

Two constraints shape the whole design.

The API is rationed. Client credentials get a points-per-hour budget, a full report's
event stream costs real points, and there is no way to get the budget back. So every
response is cached on disk under a hash of the query, re-runs of an audit cost nothing,
and the remaining budget is reported rather than discovered by being cut off.

The event volume is enormous and almost entirely worthless. A single Chromaggus kill is
thousands of events, and the answer we want from it is a table of perhaps a dozen rows.
Nothing here returns raw events to a caller: the fetchers reduce to counts, intervals and
distributions at the point of download, which keeps both memory and any transcript that
quotes them small.

Credentials come from the environment, matching the harness:

    WCL_CLIENT_ID, WCL_CLIENT_SECRET

Create them at https://www.warcraftlogs.com/api/clients/ as a v2 client. Any redirect URL
will do, since the client credentials flow never uses it.
"""

import hashlib
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

# Classic Era lives on its own host, and its zone and encounter ids are not the retail
# ones. Overridable because the same code reads retail if it is ever pointed there.
DEFAULT_HOST = os.environ.get("WCL_HOST", "https://classic.warcraftlogs.com")

CACHE_DIR = os.environ.get(
    "WCL_CACHE_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")
)


class WclError(RuntimeError):
    pass


class Wcl:
    def __init__(self, client_id, client_secret, host=DEFAULT_HOST, cache_dir=CACHE_DIR):
        if not client_id or not client_secret:
            raise WclError(
                "no credentials: set WCL_CLIENT_ID and WCL_CLIENT_SECRET. Create a v2 "
                "client at https://www.warcraftlogs.com/api/clients/"
            )
        self.client_id = client_id
        self.client_secret = client_secret
        self.host = host.rstrip("/")
        self.cache_dir = cache_dir
        self._token = None
        os.makedirs(self.cache_dir, exist_ok=True)

    @classmethod
    def from_env(cls, host=None):
        return cls(
            os.environ.get("WCL_CLIENT_ID"),
            os.environ.get("WCL_CLIENT_SECRET"),
            host or DEFAULT_HOST,
        )

    # -- auth ---------------------------------------------------------------

    def token(self):
        if self._token:
            return self._token

        body = urllib.parse.urlencode({"grant_type": "client_credentials"}).encode()
        request = urllib.request.Request(f"{self.host}/oauth/token", data=body)

        # Basic auth rather than posting the secret as form fields, so the secret never
        # appears in a query string that might be logged by something in between.
        import base64

        pair = f"{self.client_id}:{self.client_secret}".encode()
        request.add_header("Authorization", "Basic " + base64.b64encode(pair).decode())

        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                self._token = json.loads(response.read())["access_token"]
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")[:200]
            raise WclError(f"oauth failed ({error.code}): {detail}") from None

        return self._token

    # -- graphql ------------------------------------------------------------

    def query(self, document, variables=None, cache=True):
        """Run a GraphQL query, answering from disk when it has been run before.

        Cached by the query and its variables together, so a different fight or a
        different page is a different entry and nothing is ever served stale by accident.
        """
        variables = variables or {}
        key = hashlib.sha256(
            json.dumps([self.host, document, variables], sort_keys=True).encode()
        ).hexdigest()[:32]
        path = os.path.join(self.cache_dir, f"{key}.json")

        if cache and os.path.exists(path):
            with open(path, encoding="utf-8") as handle:
                return json.load(handle)

        payload = json.dumps({"query": document, "variables": variables}).encode()
        request = urllib.request.Request(f"{self.host}/api/v2/client", data=payload)
        request.add_header("Authorization", f"Bearer {self.token()}")
        request.add_header("Content-Type", "application/json")

        for attempt in range(4):
            try:
                with urllib.request.urlopen(request, timeout=120) as response:
                    body = json.loads(response.read())
                break
            except urllib.error.HTTPError as error:
                # 429 is the points budget, 5xx is theirs rather than ours. Both are
                # worth waiting out; anything else is a bug in the query and is not.
                if error.code in (429, 502, 503, 504) and attempt < 3:
                    time.sleep(5 * (attempt + 1))
                    continue
                detail = error.read().decode("utf-8", "replace")[:300]
                raise WclError(f"query failed ({error.code}): {detail}") from None
        else:
            raise WclError("query failed after retries")

        if "errors" in body:
            raise WclError(f"graphql errors: {json.dumps(body['errors'])[:300]}")

        data = body.get("data")
        if cache:
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(data, handle)
        return data

    # -- budget -------------------------------------------------------------

    def rate_limit(self):
        """Points spent and remaining this hour. Never cached, for obvious reasons."""
        data = self.query(
            "{ rateLimitData { limitPerHour pointsSpentThisHour pointsResetIn } }",
            cache=False,
        )
        info = data["rateLimitData"]
        info["remaining"] = info["limitPerHour"] - info["pointsSpentThisHour"]
        return info

    # -- discovery ----------------------------------------------------------

    def zones(self):
        """Every zone and encounter this host knows, which is how boss ids are found.

        Cached hard: it changes when Blizzard releases a raid tier, not otherwise.
        """
        data = self.query(
            """
            { worldData { expansions { id name
                zones { id name encounters { id name } } } } }
            """
        )
        return data["worldData"]["expansions"]

    def find_encounter(self, name):
        """Encounters whose name contains this, with the zone they belong to."""
        needle = name.lower()
        hits = []
        for expansion in self.zones():
            for zone in expansion["zones"] or []:
                for encounter in zone["encounters"] or []:
                    if needle in encounter["name"].lower():
                        hits.append(
                            {
                                "expansion": expansion["name"],
                                "zoneId": zone["id"],
                                "zone": zone["name"],
                                "encounterId": encounter["id"],
                                "encounter": encounter["name"],
                            }
                        )
        return hits

    def reports_for_zone(self, zone_id, limit=25, page=1):
        """Recent reports in a zone. Fights are filtered per report, not here."""
        data = self.query(
            """
            query ($zone: Int!, $limit: Int!, $page: Int!) {
              reportData { reports(zoneID: $zone, limit: $limit, page: $page) {
                data { code title startTime endTime } } }
            }
            """,
            {"zone": zone_id, "limit": limit, "page": page},
        )
        return data["reportData"]["reports"]["data"]

    # -- one report ---------------------------------------------------------

    def report_overview(self, code, encounter_id):
        """The kills of one encounter in one report, and the NPCs that took part.

        `gameID` on an actor is the creature template entry, which is what lets an
        observation join straight onto our own `creature_template` without any name
        matching. That single field is why this is worth doing against the API rather
        than against a parsed log file.
        """
        data = self.query(
            """
            query ($code: String!, $encounter: Int!) {
              reportData { report(code: $code) {
                masterData { actors(type: "NPC") { id gameID name } }
                fights(encounterID: $encounter) {
                  id encounterID name kill startTime endTime }
              } }
            }
            """,
            {"code": code, "encounter": encounter_id},
        )
        report = data["reportData"]["report"]
        return report["masterData"]["actors"], report["fights"]

    def casts(self, code, fight_id, source_id, start, end):
        """Every cast by one actor in one fight, paged to exhaustion.

        Returns the raw event dicts because the caller reduces them immediately. Nothing
        upstream of the reducers should hold on to this.
        """
        events = []
        cursor = start
        while cursor is not None:
            data = self.query(
                """
                query ($code: String!, $fight: Int!, $source: Int!,
                       $start: Float!, $end: Float!) {
                  reportData { report(code: $code) {
                    events(fightIDs: [$fight], dataType: Casts, sourceID: $source,
                           startTime: $start, endTime: $end, limit: 10000) {
                      data nextPageTimestamp } } }
                }
                """,
                {
                    "code": code,
                    "fight": fight_id,
                    "source": source_id,
                    "start": cursor,
                    "end": end,
                },
            )
            block = data["reportData"]["report"]["events"]
            events.extend(block["data"] or [])
            cursor = block["nextPageTimestamp"]
        return events

    def ability_names(self, spell_ids):
        """Resolve spell ids to names, so a diff reads as words rather than numbers."""
        names = {}
        for spell_id in sorted(set(spell_ids)):
            try:
                data = self.query(
                    "query ($id: Int!) { gameData { ability(id: $id) { id name } } }",
                    {"id": int(spell_id)},
                )
                ability = (data.get("gameData") or {}).get("ability")
                if ability:
                    names[int(spell_id)] = ability["name"]
            except WclError:
                continue
        return names
