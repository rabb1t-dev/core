"""Read and write mangosd.conf / realmd.conf by key, preserving file structure.

Values are presented and written back verbatim (including any surrounding quotes), so
the panel never has to guess quoting rules. Every save takes a timestamped backup first.
"""
import os
import re
import time
import shutil

# A plain "Key = Value" line that is not a comment.
_LINE = re.compile(r'^(\s*)([A-Za-z0-9_.]+)(\s*=\s*)(.*?)(\s*)$')


def parse(path):
    """Return a list of {key, value, lineno} for every assignment line."""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for i, raw in enumerate(f):
            line = raw.rstrip("\n")
            if line.lstrip().startswith("#"):
                continue
            m = _LINE.match(line)
            if m:
                out.append({"key": m.group(2), "value": m.group(4), "lineno": i})
    return out


def grouped(path):
    """Group settings by the token before the first dot, for display."""
    groups = {}
    for e in parse(path):
        g = e["key"].split(".")[0]
        groups.setdefault(g, []).append(e)
    return dict(sorted(groups.items()))


def save(path, changes):
    """changes: {key: new_raw_value}. Returns (backup_path, num_changed)."""
    bak = "%s.bak-%s" % (path, time.strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(path, bak)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    changed = 0
    for i, raw in enumerate(lines):
        if raw.lstrip().startswith("#"):
            continue
        m = _LINE.match(raw.rstrip("\n"))
        if m and m.group(2) in changes:
            newval = changes[m.group(2)]
            if newval != m.group(4):
                lines[i] = "%s%s%s%s\n" % (m.group(1), m.group(2), m.group(3), newval)
                changed += 1
    if changed:
        with open(path, "w", encoding="utf-8") as f:
            f.writelines(lines)
    else:
        os.remove(bak)  # nothing changed, don't litter backups
    return bak, changed
