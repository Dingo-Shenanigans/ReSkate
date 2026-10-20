"""Assemble decoded catalogs into a local library without touching user-saved animations.

Usage: python assemble_native_library.py DESTINATION SOURCE [SOURCE ...]
Copies completed payloads first, then atomically replaces catalog.json. Run again
after an ongoing exporter finishes to install the remaining clips.
"""
import json
import os
import pathlib
import shutil
import sys
import time

from make_trick_library import TRICKS


def main():
    destination = pathlib.Path(sys.argv[1]).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    families = {trick: family for family, trick, _, _ in TRICKS}
    entries = {}
    for source_name in sys.argv[2:]:
        source = pathlib.Path(source_name).resolve()
        for attempt in range(3):
            try:
                catalog = json.loads((source / "catalog.json").read_text(encoding="utf-8-sig"))
                break
            except json.JSONDecodeError:
                if attempt == 2:
                    raise
                time.sleep(0.1)
        for entry in catalog:
            filename = entry["file"]
            path = pathlib.Path(filename)
            if path.name != filename or path.suffix != ".clip" or not (source / path).resolve().is_relative_to(source):
                raise ValueError("Invalid local clip path")
            if not entry.get("asset") and entry.get("trick") in families:
                entry["asset"] = f"animation/dingo/c_proto_onb_trick_{families[entry['trick']]}_{entry['pop']}"
            shutil.copy2(source / filename, destination / filename)
            entries[filename] = entry
        if (source / "native-catalog.json").is_file():
            shutil.copy2(source / "native-catalog.json", destination / "native-catalog.json")
    rows = sorted(entries.values(), key=lambda e: (e["category"], e["label"], {"low": 0, "mid": 1, "high": 2}.get(e["pop"], 3)))
    temp = destination / "catalog.json.tmp"
    temp.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    os.replace(temp, destination / "catalog.json")
    print(f"Installed {len(rows)} clips; {sum(bool(e.get('preview_only')) for e in rows)} preview-only phases")


if __name__ == "__main__":
    main()
