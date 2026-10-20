"""Index a local `reskate_cli find-name <game> animation/dingo/ 100000` dump.

Categories and kinds are hints from asset names, not decoded engine types. Keeping
controllers and parameters visible avoids claiming that everything is a clip.
"""
import collections
import json
import pathlib
import sys


def classify(name):
    n = name.lower()
    if any(s in n for s in ("bool.", "float.", "enum.", "enumgs.", "int.")):
        return "Graph/Parameters", "parameter candidate"
    if "transition" in n or "_to_" in n:
        return "Graph/Transitions", "transition candidate"
    if ".node" in n or ".conduit" in n:
        return "Graph/States", "state candidate"
    if "superlayer" in n or "layer" in n:
        return "Graph/Layers", "layer candidate"
    kind = "clip candidate" if n.startswith("c_") else "controller or other asset"
    if "skatepedia" in n:
        return "Skatepedia/Grinds" if "grind" in n else "Skatepedia/Other", kind
    if "grind" in n or "slide" in n:
        phase = "Lock-in" if any(s in n for s in ("lockin", "lock_in", "entry", "_into")) else "Loops" if "loop" in n else "Exits" if any(s in n for s in ("exit", "popout", "pop_out")) else "Other"
        return "On board/Grinds/" + phase, kind
    if "trick" in n or "ollie" in n or "flip" in n:
        return "On board/Tricks/Nollie" if "_n_" in n or "nollie" in n else "On board/Tricks/Other", kind
    if "manual" in n:
        return "On board/Manuals", kind
    if any(s in n for s in ("ofb", "offboard", "off_board")):
        return "Off board/Movements", kind
    if any(s in n for s in ("push", "brake", "pump", "carv", "cruise", "roll", "ride")):
        return "On board/Locomotion", kind
    if any(s in n for s in ("idle", "fidget", "look", "corrective", "adjust")):
        return "Small movements/Idle and corrections", kind
    if any(s in n for s in ("bail", "wipeout", "stumble", "fall")):
        return "Bails/Recovery", kind
    if "emote" in n or "gesture" in n:
        return "Gestures/Emotes", kind
    return "Other/" + ("Clip candidates" if kind == "clip candidate" else "Unclassified"), kind


def main():
    entries = {}
    for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("ebx animation/dingo/"):
            continue
        asset = line[4:].strip()
        category, kind = classify(asset.removeprefix("animation/dingo/"))
        entries[asset] = {"asset": asset, "category": category, "kind": kind}
    if not entries:
        raise SystemExit("No animation/dingo EBX assets in the input")
    out = pathlib.Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    rows = sorted(entries.values(), key=lambda e: (e["category"], e["asset"]))
    out.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    print(json.dumps({"assets": len(rows), "category_hints": dict(collections.Counter(e["category"] for e in rows)), "scope": "animation/dingo EBX inventory; type and category inferred from names"}, indent=2))


if __name__ == "__main__":
    main()
