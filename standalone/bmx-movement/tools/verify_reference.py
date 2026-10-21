"""Inventory pinned IL2CPP metadata and reject stale movement evidence.

This tool parses generated declarations and PE bytes. It never imports a game
assembly, executes the game, or treats Cpp2IL's empty bodies as recovered code.
Only Python's standard library is required.
"""
from pathlib import Path
from collections import Counter
import argparse
import hashlib
import json
import re
import struct
import sys

REPO = Path(__file__).resolve().parents[3]
MODULE = Path(__file__).resolve().parents[1]
CURRENT = REPO / "build/bmx-movement-current"
OLD = REPO / "build/bmx-streets-controls"
METHOD = re.compile(
    r'\[Address\(RVA = "(0x[0-9a-fA-F]+)", Offset = "(0x[0-9a-fA-F]+)", Length = "(0x[0-9a-fA-F]+)"\)\]'
    r'\s*(?:\[[^\n]+\]\s*)*([^\n]+\{ \}[^\n]*)')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf8"))


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("Not a PE")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe+4] != b"PE\0\0":
            raise ValueError("Invalid PE signature")
        opt = pe+24
        table = opt+struct.unpack_from("<H", data, pe+20)[0]
        self.base = struct.unpack_from("<Q", data, opt+24)[0]
        self.sections = []
        for i in range(struct.unpack_from("<H", data, pe+6)[0]):
            at = table+40*i
            virtual_size, rva, raw_size, offset = struct.unpack_from("<4I", data, at+8)
            if offset+raw_size > len(data):
                raise ValueError("PE raw section exceeds file")
            self.sections.append((rva, raw_size, offset))

    def read(self, rva, length):
        for start, size, offset in self.sections:
            if start <= rva and rva+length <= start+size:
                pos = offset+rva-start
                return self.data[pos:pos+length], pos
        raise ValueError(f"RVA {rva:#x}+{length:#x} is not file backed")


def family(relative):
    text = str(relative).lower()
    if any(x in text for x in ("trick", "flick", "tailwhip", "barspin")):
        return "tricks"
    if any(x in text for x in ("grind", "peg", "manual", "nose", "manny")):
        return "manuals_grinds"
    if any(x in text for x in ("land", "collision", "ragdoll", "bail", "reviv")):
        return "landing_collision_recovery"
    if any(x in text for x in ("input", "gamepad", "controls")):
        return "input"
    if any(x in text for x in ("anim", "motion", "rider", "ik", "poser", "human", "body")):
        return "rider_animation"
    return "bike_physics_and_state"


def metadata_inventory(pe):
    base = CURRENT / "cpp2il-addresses/DiffableCs"
    scopes = [base / "Assembly-CSharp/MashBox/BMX_Physics_Development",
              base / "MashBox.Core.Runtime/MashBox/Core/Runtime/Physics/Vehicle",
              base / "MashBox.Core.Runtime/MashBox/Core/Runtime/GrindPoseData"]
    records = []
    for scope in scopes:
        if not scope.is_dir():
            raise FileNotFoundError(scope)
        for path in sorted(scope.rglob("*.cs")):
            text = path.read_text(encoding="utf8")
            namespace = re.search(r"^namespace ([^;]+);", text, re.M)
            entry = {"class": path.stem,
                     "namespace": namespace.group(1) if namespace else None,
                     "metadata_file": path.relative_to(base).as_posix(),
                     "metadata_sha256": sha(path.read_bytes()),
                     "family": family(path.relative_to(base)), "methods": []}
            for match in METHOD.finditer(text):
                rva, offset, length = [int(x, 16) for x in match.group(1, 2, 3)]
                declaration = match.group(4).strip()
                name = re.search(r"([^\s(]+)\(", declaration)
                if not name:
                    continue
                raw, checked_offset = pe.read(rva, length)
                if checked_offset != offset:
                    raise ValueError(f"Cpp2IL RVA/file-offset mismatch {path}:{name.group(1)}")
                entry["methods"].append({"name": name.group(1), "declaration": declaration,
                    "rva": hex(rva), "preferred_va": hex(pe.base+rva), "file_offset": offset,
                    "native_length": length, "native_sha256": sha(raw),
                    "metadata_line": text.count("\n", 0, match.start())+1,
                    "current_native_behavior": "not_recovered_by_metadata_inventory"})
            records.append(entry)
    return records, [p.relative_to(base).as_posix() for p in scopes]


def old_method_checks(pe, inventory):
    methods = []
    rider = load(OLD / "rider-input-native-slice-manifest.json")
    for m in rider["methods"]:
        methods.append((m["class"], m, rider["source_sha256"]))
    wheel = load(OLD / "wheel-ordinary-method-contract.json")
    for m in wheel["methods"]:
        methods.append(("WheelPhysics", m, rider["source_sha256"]))
    results = []
    for cls, old, old_source in methods:
        candidates = [(c, m) for c in inventory if c["class"] == cls
                      for m in c["methods"] if m["name"] == old["name"]]
        if old.get("metadata_path"):
            suffix = old["metadata_path"].replace("\\", "/").split("/DiffableCs/")[-1]
            narrowed = [(c, m) for c, m in candidates if c["metadata_file"] == suffix]
            if narrowed:
                candidates = narrowed
        row = {"class": cls, "method": old["name"], "historical_source_sha256": old_source,
               "historical_rva": hex(old["rva"]), "historical_native_length": old["native_length"],
               "historical_native_sha256": old["native_sha256"]}
        try:
            raw, _ = pe.read(old["rva"], old["native_length"])
            row["same_rva_current_sha256"] = sha(raw)
            row["same_rva_bytes_match"] = sha(raw) == old["native_sha256"]
        except ValueError:
            row["same_rva_bytes_match"] = False
        row["current_metadata_matches"] = len(candidates)
        if len(candidates) == 1:
            _, current = candidates[0]
            row["current_rva"] = current["rva"]
            row["current_native_length"] = current["native_length"]
            row["current_native_sha256"] = current["native_sha256"]
            row["same_named_method_bytes_match"] = (old["native_length"] == current["native_length"]
                and old["native_sha256"] == current["native_sha256"])
        row["status"] = "historical_only_requires_current_native_analysis"
        results.append(row)
    return results


def source_checks():
    pinned = load(CURRENT / "source/manifest.json")
    results = []
    for f in pinned["files"]:
        snapshot = Path(f["snapshot_path"])
        live = Path(f["installed_path"])
        if sha(snapshot.read_bytes()) != f["sha256"]:
            raise ValueError("Pinned snapshot digest mismatch")
        current_hash = sha(live.read_bytes())
        results.append(f | {"live_sha256": current_hash,
                       "live_matches_snapshot": current_hash == f["sha256"],
                       "live_last_write_ns": live.stat().st_mtime_ns})
    return results


def authored_sources():
    records = []
    for report_name in ["physics-reference.json", "physics-typed-configs.json"]:
        path = OLD / report_name
        for source in load(path)["sources"]:
            p = Path(source["path"])
            raw = p.read_bytes() if p.exists() else b""
            records.append({"historical_report": report_name, "source_path": str(p),
                            "historical_sha256": source["sha256"],
                            "live_sha256": sha(raw) if raw else None,
                            "live_matches_historical": bool(raw) and sha(raw) == source["sha256"],
                            "status": "historical_authored_configuration_not_current_runtime_tuning"})
    return records


def callee_identities(pe):
    """Map explicit call-site targets requested during current native analysis."""
    wanted = {0x4d05be0, 0x4d07990, 0x4d072b0, 0x4cdfc40, 0x4d07ae0,
              0x4d769e0, 0x4d773d0, 0x4d060e0, 0x4d01160, 0x2e573a0,
              0x895f80, 0x8d1020, 0x8d13e0}
    base = CURRENT / "cpp2il-addresses/DiffableCs"
    paths = ["UnityEngine.CoreModule/UnityEngine/Transform.cs",
             "UnityEngine.CoreModule/UnityEngine/Quaternion.cs",
             "UnityEngine.CoreModule/UnityEngine/Time.cs",
             "UnityEngine.PhysicsModule/UnityEngine/Rigidbody.cs",
             "MashBox.Core.Runtime/MashBox/Core/Runtime/SmartData/PlayerSmartFloat.cs",
             "Assembly-CSharp/MashBox/BMX_Physics_Development/SpinSystem.cs",
             "Assembly-CSharp/MashBox/BMX_Physics_Development/BMXCollisionHandler.cs"]
    found = {}
    for rel in paths:
        path = base / rel
        text = path.read_text(encoding="utf8")
        for match in METHOD.finditer(text):
            rva, offset, length = [int(x, 16) for x in match.group(1, 2, 3)]
            if rva not in wanted or "(" not in match.group(4):
                continue
            raw, checked_offset = pe.read(rva, length)
            if checked_offset != offset:
                raise ValueError("Callee file offset mismatch")
            found[rva] = {"rva": hex(rva), "declaration": match.group(4).strip(),
                          "metadata_file": rel, "metadata_sha256": sha(path.read_bytes()),
                          "metadata_line": text.count("\n", 0, match.start())+1,
                          "native_length": length, "native_sha256": sha(raw),
                          "authority": "Cpp2IL-metadata-derived-identity-only"}
    if set(found) != wanted:
        raise ValueError(f"Missing requested callee identities: {wanted-set(found)}")
    return [found[key] for key in sorted(found)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=MODULE / "reference/evidence.json")
    args = parser.parse_args()
    sources = source_checks()
    pe = PE((CURRENT / "source/GameAssembly.dll").read_bytes())
    inventory, scopes = metadata_inventory(pe)
    comparisons = old_method_checks(pe, inventory)
    families = Counter(c["family"] for c in inventory)
    report = {"schema": "bmx-movement-reference-v1", "sources": sources,
        "source_state": "live_matches_pinned_pair" if all(f["live_matches_snapshot"] for f in sources)
                        else "installed_files_drifted_after_snapshot",
        "metadata_generator": {"tool": "Cpp2IL", "version": "2022.1.0-pre-release.21",
                               "sha256": sha((OLD / "Cpp2IL.exe").read_bytes()),
                               "processors": ["attributeinjector"], "format": "diffable-cs",
                               "log": "build/bmx-movement-current/cpp2il-addresses.log"},
        "search_boundary": scopes,
        "coverage": {"metadata_files": len(inventory),
                     "addressed_methods": sum(len(c["methods"]) for c in inventory),
                     "metadata_file_families": dict(sorted(families.items())),
                     "current_behavior_recovered_by_this_script": 0,
                     "historical_methods_checked": len(comparisons),
                     "historical_same_rva_matches": sum(x["same_rva_bytes_match"] for x in comparisons),
                     "historical_same_named_method_matches": sum(x.get("same_named_method_bytes_match", False) for x in comparisons)},
        "historical_native_comparison": comparisons, "historical_authored_sources": authored_sources(),
        "current_native_callee_metadata": callee_identities(pe),
        "classes": inventory,
        "remaining_behavior": {"bike_physics": "Current native force/friction/suspension branches require analysis and runtime calibration.",
            "rider": "Metadata establishes IK and animation interfaces; target skeleton and current live IK weights unverified.",
            "tricks": "Trick gating, clip timing, combo rules, cancellation and physical contacts need current native/runtime evidence.",
            "manuals_grinds": "Authored tuning is historical; balance, grind constraints and exit transitions unverified in current build.",
            "landing_bail": "Land, collision and revival method identities inventoried; runtime thresholds and outcomes unobserved."},
        "limits": ["Address-bearing Cpp2IL declarations are metadata stubs, not recovered C# method bodies.",
                   "Different native bytes can result from relocated references/compiler changes and do not alone prove different movement semantics.",
                   "Matching native bytes alone do not verify settings, referenced data, callees, ABI or live behavior.",
                   "Namespace inventories are bounded candidates; this is not a proof of all movement-related code coverage.",
                   "Preferred addresses are not callable engine-independent interfaces; no game assembly is loaded or executed.",
                   "Historical authored settings are not confirmed current runtime tuning.",
                   "Standalone C++ algorithms are a reimplementation and do not yet demonstrate full behavioral parity."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"output": str(args.output), "source_state": report["source_state"],
                      "coverage": report["coverage"]}, indent=2))
    return 0 if report["source_state"] == "live_matches_pinned_pair" else 2


if __name__ == "__main__":
    sys.exit(main())
