"""Read current authored BMX/Tricking input literals without executing game code."""
from pathlib import Path
import hashlib
import json
import struct

MODULE = Path(__file__).resolve().parents[1]
REPO = MODULE.parents[1]
SOURCE = REPO / "build/bmx-movement-current/source/global-metadata.dat"
OLD = REPO / "build/bmx-streets-controls/metadata-input-inventory.json"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def input_assets(data):
    h = struct.unpack_from("<8I", data)
    if h[:2] != (0xFAB11BAF, 31):
        raise ValueError("Unsupported metadata magic/version")
    off, size, payload, payload_size = h[2:6]
    if size % 8 or off+size > len(data) or payload+payload_size > len(data):
        raise ValueError("Invalid metadata literal section")
    found = []
    for i in range(size//8):
        length, start = struct.unpack_from("<II", data, off+8*i)
        if start+length > payload_size:
            raise ValueError("Literal exceeds payload")
        raw = data[payload+start:payload+start+length]
        if b'"maps"' not in raw or b'"bindings"' not in raw:
            continue
        try:
            obj = json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            continue
        selected = [m for m in obj.get("maps", []) if m.get("name") in ("BMX", "Tricking")]
        if selected:
            found.append({"literal_index": i, "file_offset": payload+start,
                          "literal_bytes": length, "literal_sha256": sha(raw),
                          "name": obj.get("name"), "maps": selected,
                          "control_schemes": obj.get("controlSchemes", [])})
    return found


def normalize(m):
    return {"actions": [{k: v for k, v in x.items() if k != "id"} for x in m["actions"]],
            "bindings": [{k: v for k, v in x.items() if k != "id"} for x in m["bindings"]]}


def main():
    data = SOURCE.read_bytes()
    pinned = json.loads((SOURCE.parent / "manifest.json").read_text(encoding="utf8"))
    expected = next(x for x in pinned["files"] if x["snapshot_path"] == str(SOURCE))["sha256"]
    if sha(data) != expected:
        raise ValueError("Pinned metadata digest changed")
    assets = input_assets(data)
    if not assets:
        raise ValueError("No BMX/Tricking maps found")
    old = json.loads(OLD.read_text(encoding="utf8"))
    old_maps = {m["name"]: m for a in old["input_maps"] for m in a["maps"]
                if m["name"] in ("BMX", "Tricking")}
    comparisons = []
    for asset in assets:
        for m in asset["maps"]:
            bindings = [{"action": b.get("action"), "path": b.get("path"),
                         "processors": b.get("processors"), "interactions": b.get("interactions")}
                        for b in m["bindings"]]
            comparisons.append({"map": m["name"], "actions": len(m["actions"]),
                "bindings": len(m["bindings"]),
                "matches_historical_except_ids": m["name"] in old_maps and normalize(m) == normalize(old_maps[m["name"]]),
                "modifier_actions": [a["name"] for a in m["actions"] if m["name"] == "Tricking" and a["name"] != "Trick Stick"],
                "modifier_bindings": [b for b in bindings if m["name"] == "Tricking" and b["action"] != "Trick Stick"],
                "trick_stick_bindings": [b for b in bindings if b["action"] == "Trick Stick"],
                "stick_bindings": [b for b in bindings if "stick" in (b["path"] or "").lower()]})
    report = {"schema": "bmx-current-authored-controls-v1", "source": str(SOURCE),
              "source_sha256": sha(data), "assets": assets, "summary": comparisons,
              "historical_catalog": {"status": "historical_unverified_for_current_build",
                 "historical_slot_count": 58,
                 "reason": "Old DLL, metadata and serialized movement assets have different digests; current trickset bindings do not verify slot catalog or animation parity."},
              "limits": ["Authored InputSystem JSON literal definitions only; active enabled action map, rebinding and runtime device interpretation remain unobserved.",
                         "Processors/interactions establish authored input transforms, not native trick-mask implementation or movement behavior.",
                         "No game DLL is imported or executed."]}
    out = MODULE / "reference/current-controls.json"
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"output": str(out), "source_sha256": sha(data), "summary": comparisons}, indent=2))


if __name__ == "__main__":
    main()
