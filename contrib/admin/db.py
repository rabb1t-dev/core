"""Database access for the vmangos admin panel.

Connection credentials are never stored in the panel. They are parsed at runtime out of
mangosd.conf (the same source mangosd itself uses), so there is one place that holds them.
"""
import re
import pymysql
import pymysql.cursors

_KEYS = {
    "world": "WorldDatabase.Info",
    "char": "CharacterDatabase.Info",
    "login": "LoginDatabase.Info",
}


def _parse_info(conf_path, key):
    pat = re.compile(r'^\s*' + re.escape(key) + r'\s*=\s*"([^"]*)"')
    with open(conf_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                p = m.group(1).split(";")
                if len(p) >= 5:
                    return {
                        "host": p[0],
                        "port": int(p[1]),
                        "user": p[2],
                        "password": p[3],
                        "db": p[4],
                    }
    raise RuntimeError("%s not found in %s" % (key, conf_path))


class DB:
    def __init__(self, conf_path):
        self.conf_path = conf_path
        self._cache = {}

    def info(self, which):
        if which not in self._cache:
            self._cache[which] = _parse_info(self.conf_path, _KEYS[which])
        return self._cache[which]

    def conn(self, which):
        i = self.info(which)
        return pymysql.connect(
            host=i["host"], port=i["port"], user=i["user"],
            password=i["password"], database=i["db"],
            charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor,
            autocommit=True, connect_timeout=5, read_timeout=10, write_timeout=10,
        )

    def query(self, which, sql, args=None):
        with self.conn(which) as c:
            with c.cursor() as cur:
                cur.execute(sql, args or ())
                return cur.fetchall()

    def exec(self, which, sql, args=None):
        with self.conn(which) as c:
            with c.cursor() as cur:
                cur.execute(sql, args or ())
                return cur.rowcount
