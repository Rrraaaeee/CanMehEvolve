#!/usr/bin/env python3
"""Build one patch holding the best rewrite of every region any Phase 1 run improved.

For each region, this picks the rewrite with the largest confirmed gain over its own
parent, then splices those rewrites together, largest gain first. Chosen after reading
the Phase 1 results, so report what it measures apart from that round.

Usage, from the repository root:
  scripts/build_max_patch.py <phase1-directory> <work-directory> <output-directory>
Writes max.patch, selection.csv and the replacement files to the output directory.
"""
import csv
import glob
import json
import os
import re
import subprocess
import sys

HARNESS = "./build/CanMehHarness"
FENCE = re.compile(r"```([^\n]*)\n(.*?)```", re.S)


def split_reply(text):
    """The same choice as splitReply in src/Search.cpp."""
    code = includes = None
    for info, body in FENCE.findall(text):
        if "include" in info:
            includes = includes if includes is not None else body
        elif code is None:
            code = body
    return (code if code and "{" in code else None), (includes or "")


def candidates(root):
    best = {}
    for path in sorted(glob.glob(os.path.join(root, "*-*", "results.jsonl"))):
        run = os.path.dirname(path)
        with open(path) as handle:
            records = {r["id"]: r for r in map(json.loads, handle) if r.get("ok")}
        for record in records.values():
            if not record.get("improved_on_parent"):
                continue
            parent = 1.0 if record["parent"] == 0 else records[record["parent"]]["ratio"]
            gain = parent / record["ratio"] - 1
            if gain > best.get(record["region"], {"gain": 0.0})["gain"]:
                best[record["region"]] = {"gain": gain, "run": run, "record": record}
    return sorted(best.items(), key=lambda item: -item[1]["gain"])


def splice(work, region, replacement, includes, parent, out):
    command = [HARNESS, "splice", "--work", work, "--region", str(region),
               "--replacement", replacement, "--out", out]
    if includes:
        command += ["--includes", includes]
    if parent:
        command += ["--patch", parent]
    done = subprocess.run(command, capture_output=True, text=True)
    return json.loads(done.stdout) if done.stdout.strip() else {"ok": False, "error": done.stderr}


def main():
    root, work, out = sys.argv[1:4]
    pieces = os.path.join(out, "pieces")
    os.makedirs(pieces, exist_ok=True)
    current = ""
    with open(os.path.join(out, "selection.csv"), "w", newline="") as handle:
        table = csv.writer(handle)
        table.writerow(["region", "file", "label", "run", "generation", "gain_over_parent", "applied"])
        for region, chosen in candidates(root):
            record = chosen["record"]
            with open(os.path.join(chosen["run"], "patches", f"gen_{record['generation']}.repl")) as reply:
                code, includes = split_reply(reply.read())
            applied = False
            if code:
                replacement = os.path.join(pieces, f"region-{region}.cpp")
                with open(replacement, "w") as piece:
                    piece.write(code)
                include_file = ""
                if includes.strip():
                    include_file = os.path.join(pieces, f"region-{region}.inc")
                    with open(include_file, "w") as piece:
                        piece.write(includes)
                following = os.path.join(pieces, f"after-{region}.patch")
                reply = splice(work, region, replacement, include_file, current, following)
                applied = bool(reply.get("ok"))
                if applied:
                    current = following
                else:
                    print(f"region {region} ({record['label']}) not applied: {reply}", file=sys.stderr)
            table.writerow([region, record["file"], record["label"], os.path.basename(chosen["run"]),
                            record["generation"], f"{chosen['gain']:.5f}", applied])
    if not current:
        sys.exit("no rewrite could be applied")
    with open(current) as source, open(os.path.join(out, "max.patch"), "w") as target:
        target.write(source.read())


if __name__ == "__main__":
    main()
