"""Snapshot the installed IL2CPP pair without executing game code."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[3]
INSTALL = Path(r"D:\SteamLibrary\steamapps\common\BMX Streets")
OUT = ROOT / "build/bmx-movement-current/source"
FILES = {"GameAssembly.dll": INSTALL / "GameAssembly.dll",
         "global-metadata.dat": INSTALL / "BMX Streets_Data/il2cpp_data/Metadata/global-metadata.dat"}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def main():
    snapshots = {name: path.read_bytes() for name, path in FILES.items()}
    for name, path in FILES.items():
        if digest(path.read_bytes()) != digest(snapshots[name]):
            raise RuntimeError("Installed files changed during snapshot; try after update completes")
    OUT.mkdir(parents=True, exist_ok=True)
    records = []
    for name, data in snapshots.items():
        target = OUT / name
        if target.exists() and target.read_bytes() != data:
            raise RuntimeError(f"Refusing to overwrite differing snapshot {target}")
        target.write_bytes(data)
        records.append({"installed_path": str(FILES[name]), "snapshot_path": str(target),
                        "sha256": digest(data), "bytes": len(data),
                        "last_write_ns": FILES[name].stat().st_mtime_ns})
    manifest = {"authority": "local-shipped-artifact", "files": records,
                "limits": ["Stable read comparison establishes coherent observed files, not Steam publisher provenance.",
                           "The game was not executed."]}
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    print(json.dumps(manifest, indent=2))

if __name__ == "__main__":
    main()
