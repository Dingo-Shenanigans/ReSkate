"""Extract selected LOCAL UnityFS BMX bundles; never execute or modify the source game.

Requires UnityPy in the workspace's build/bmx-unity-deps (or normal Python path).
Outputs are ignored local references, not a redistributable ReSkate content pack.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "build" / "bmx-unity-deps"))


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value)[:160] or "unnamed"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundles", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "reference")
    parser.add_argument("--inventory-only", action="store_true")
    args = parser.parse_args()
    import UnityPy
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"tool": f"UnityPy {UnityPy.__version__}", "purpose": "local reference extraction", "sources": []}
    for source in args.bundles:
        source = source.resolve(strict=True)
        destination = output / safe_name(source.stem)
        destination.mkdir(exist_ok=True)
        content = source.read_bytes()
        record = {"path": str(source), "sha256": hashlib.sha256(content).hexdigest(), "size": len(content), "objects": []}
        env = UnityPy.load(str(source))
        for obj in env.objects:
            kind = obj.type.name
            if kind not in {"Mesh", "GameObject", "Transform", "MeshFilter", "MeshRenderer", "SkinnedMeshRenderer", "AnimationClip", "Avatar", "Material", "Texture2D"}:
                continue
            entry = {"path_id": obj.path_id, "type": kind}
            record["objects"].append(entry)
            try:
                data = obj.read()
                name = getattr(data, "m_Name", "")
                entry["name"] = name
                stem = f"{safe_name(name or kind)}_{obj.path_id}"
                if args.inventory_only:
                    continue
                if kind == "Mesh":
                    exported = data.export()
                    (destination / f"{stem}.obj").write_text(exported, encoding="utf-8")
                    entry["export"] = f"{destination.name}/{stem}.obj"
                    entry["vertices"] = getattr(data, "m_VertexCount", None)
                elif kind == "Texture2D":
                    data.image.save(destination / f"{stem}.png")
                    entry["export"] = f"{destination.name}/{stem}.png"
                if kind not in {"Texture2D", "Mesh"}:
                    tree = obj.read_typetree()
                    (destination / f"{stem}.json").write_text(json.dumps(tree, indent=2), encoding="utf-8")
                    entry["export"] = f"{destination.name}/{stem}.json"
            except Exception as error:
                entry["error"] = f"{type(error).__name__}: {error}"
        manifest["sources"].append(record)
        print(f"{source.name}: {len(record['objects'])} relevant objects", flush=True)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    failures = sum("error" in obj for source in manifest["sources"] for obj in source["objects"])
    print(f"Manifest: {output / 'manifest.json'}; object errors: {failures}")


if __name__ == "__main__":
    main()
