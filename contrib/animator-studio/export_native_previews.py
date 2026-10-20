"""Decode selected native assets for studio editing, without guessing gameplay bindings.

Usage: python export_native_previews.py GAME OUTPUT ASSET [ASSET ...] --probe PROBE_EXE --dof-names VERIFIED_BINDINGS_JSON
The catalog marks these entries preview_only; no takeoff/landing is inferred for a grind.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import argparse

from make_trick_library import CLI, convert, joint_hashes, write_clip
from native_piecewise import read_native_vectors


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game')
    parser.add_argument('output')
    parser.add_argument('assets',nargs='+')
    parser.add_argument('--probe',required=True,type=pathlib.Path)
    parser.add_argument('--dof-names',required=True,type=pathlib.Path)
    args=parser.parse_args()
    game,output,assets=args.game,args.output,args.assets
    dof_names={int(key):value for key,value in json.loads(args.dof_names.read_text()).items()}
    folder = pathlib.Path(output)
    folder.mkdir(parents=True, exist_ok=True)
    hashes = joint_hashes(game)
    catalog = []
    with tempfile.TemporaryDirectory() as tmp:
        for asset in assets:
            if not asset.startswith("animation/dingo/") or ".." in asset:
                raise ValueError("Expected a native animation/dingo asset")
            name = asset.rsplit("/", 1)[-1]
            fbx = os.path.join(tmp, name + ".fbx")
            subprocess.run([CLI, "animation-export", game, asset, fbx], check=True, capture_output=True, text=True)
            native_folder=pathlib.Path(tmp)/(name+'-native')
            vectors=read_native_vectors(args.probe,game,asset,native_folder,dof_names)
            native_looping=json.loads((native_folder/'controller.json').read_text())['instances'][0]['object']['fields']['IsLooping']
            times, frames, board, _, _ = convert(fbx, hashes, vectors)
            write_clip(folder / (name + ".clip"), times, frames, board, -1, -1)
            catalog.append({"category": "Grinds (preview only)", "label": name.removeprefix("c_proto_onb_grind_").replace("_", " "),
                            "trick": "native:" + name, "pop": "", "file": name + ".clip", "asset": asset, "preview_only": True,
                            "position_codec": "native-piec-v1-extent", "native_looping": native_looping})
            (folder / "catalog.json").write_text(json.dumps(catalog, indent=1), encoding="utf-8")
            print(f"Preview decoded: {asset}, {len(times)} frames", flush=True)


if __name__ == "__main__":
    main()
