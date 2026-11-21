#!/usr/bin/env python3
"""Writes raw/NOTICE: the crates libphotos_raw.a is built from, with their
licences (cargo metadata, normal dependencies for the air/OS targets).

    tools/raw-host.sh metadata --format-version 1 --locked > /tmp/meta.json
    python3 tools/raw-notice.py /tmp/meta.json > raw/NOTICE

Run it again whenever raw/Cargo.lock changes.
"""
import json
import sys

meta = json.load(open(sys.argv[1]))
packages = {p["id"]: p for p in meta["packages"]}
nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
root = meta["resolve"]["root"]

# Everything reachable through normal (not build, not dev) dependencies that
# is not only for another operating system.
OTHER = ("windows", "wasm", "macos", "ios", "android", "apple", "redox", "hermit", "wasi", "emscripten")


def for_haiku(kinds):
    for kind in kinds:
        if kind["kind"] is not None:
            continue
        target = kind.get("target")
        if target is None:
            return True
        if not any(word in target for word in OTHER):
            return True
        if "not(" in target:
            return True
    return False


seen, queue = set(), [root]
while queue:
    current = queue.pop()
    if current in seen:
        continue
    seen.add(current)
    for dep in nodes[current]["deps"]:
        if for_haiku(dep["dep_kinds"]):
            queue.append(dep["pkg"])

print("""photos-raw (libphotos_raw.a) is built from the crates below. A program that
links it carries their code and owes their notices; each crate's licence text
is in its source (the cargo registry, or the repository named here).

LightCraft (the lightcraft-* crates, https://github.com/airos-works/lightcraft,
a fork of https://github.com/storytold/lightcraft) is licensed MIT OR
Apache-2.0, copyright its contributors; see its LICENSE-MIT, LICENSE-APACHE
and NOTICE. The final link keeps only what Photos calls (the RAW decoder, the
codecs and the CPU pipeline), so the binary holds a subset of this list.
The HEIF decoder (lightcraft-heif, heic-rs) implements HEVC, which is covered
by patents in some countries: the "heif" feature of the crate leaves it out.

name version: licence (source)
""")
rows = []
for pid in seen:
    if pid == root:
        continue
    p = packages[pid]
    source = p.get("repository") or (p.get("source") or "").split("#")[0] or "local"
    rows.append((p["name"].lower(), "%s %s: %s (%s)" % (p["name"], p["version"], p.get("license") or "see the crate", source)))
for _, row in sorted(rows):
    print(row)
