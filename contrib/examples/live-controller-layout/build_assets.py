"""Adapt the original mod's authoring sources to the namespaced UI-values API.

Requires the complete Live Controller 2.1.2 source tree and its Studio tooling.
Only writes the requested output; never installs a mod or launches the game.
The original authoring sources and locally generated game assets stay outside git.
"""
import argparse
import json
from pathlib import Path
import sys
import types
import zipfile

PLUGIN = "Live_Controller_Layout"
OLD_SYMBOLS = "{'left': 0xec710101, 'top': 0xec710102, 'width': 0xec710103}"


def symbol(name):
    value = 5381
    for byte in f"ReSkate.Plugin.{PLUGIN}.{name}".encode("ascii"):
        value = ((value * 33) ^ byte) & 0xffffffff
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builders", required=True, type=Path, help="Original ReplayControllerOverlay/tools folder")
    parser.add_argument("--game", required=True, type=Path, help="Supported game installation, read only")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    builders, output = args.builders.resolve(), args.output.resolve()
    if builders == output or builders in output.parents or output in builders.parents:
        parser.error("output must be separate from the source tree")
    if output.exists() and any(output.iterdir()):
        parser.error("output must be new or empty; existing build artifacts are not overwritten")
    source = (builders / "menu_graph.py").read_text(encoding="utf-8")
    if source.count(OLD_SYMBOLS) != 1:
        parser.error("this is not the expected 2.1.2 Live Controller graph builder")
    symbols = {key: symbol(key) for key in ("left", "top", "width")}
    adapted = source.replace(OLD_SYMBOLS, repr(symbols))
    sys.path.insert(0, str(builders))
    module = types.ModuleType("menu_graph")
    module.__file__ = str(builders / "menu_graph.py")
    sys.modules["menu_graph"] = module
    exec(compile(adapted, module.__file__, "exec"), module.__dict__)
    from build_native_layout import NativeLayoutBuild
    build = NativeLayoutBuild(args.game.resolve(), output=output)
    build.title = "Live Controller - Plugin UI Values"
    build.version = "0.1.0-ui-values"
    build.description = "Recorded replay inputs with plugin-owned live layout float models. Requires ReSkate's ui.models API."
    build.run()
    here = Path(__file__).resolve().parent
    # Keep the packaged graph source identical to the graph actually compiled.
    # No original source file, installed game file or saved preference is changed.
    for archive in [output / (build.title + ".zip")]:
        temporary = archive.with_suffix(".bridge.tmp")
        with zipfile.ZipFile(archive) as old, zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as new:
            for item in old.infolist():
                data = old.read(item.filename)
                if item.filename.endswith("/tools/menu_graph.py"):
                    data = adapted.encode("utf-8")
                if item.filename == "README.md":
                    data = (here.parents[1] / "live-ui-values.md").read_bytes()
                new.writestr(item, data)
            for path in (here / "reskate-plugin.json", here / "plugin/main.lua"):
                new.write(path, f"Mods/{PLUGIN}/{path.relative_to(here).as_posix()}")
            new.writestr("Source/ui-value-symbols.json", json.dumps(symbols, indent=2))
            new.write(Path(__file__), "Source/build_assets.py")
        temporary.replace(archive)
        with zipfile.ZipFile(archive) as check:
            if check.testzip() is not None:
                raise ValueError("adapted package failed its ZIP integrity check")
    (output / "ui-value-symbols.json").write_text(json.dumps(symbols, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
