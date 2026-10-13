#!/usr/bin/env python3
"""Compare baseline/patched Windows codec tests in an isolated Proton Wine prefix."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", required=True, type=Path, help="wine executable from an installed Proton")
    parser.add_argument("--baseline", required=True, type=Path, help="oodle_path test built with the original cas_codec.cpp")
    parser.add_argument("--patched", required=True, type=Path, help="oodle_path test built with the patched cas_codec.cpp")
    parser.add_argument("--codec", required=True, type=Path, help="test-only oo2core_9_win64.dll fixture, never the game's DLL")
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("The malformed-filename reproduction requires a Unix host")
    for path in (args.wine, args.baseline, args.patched, args.codec):
        if not path.is_file():
            parser.error(f"Missing fixture executable/library: {path}")
    scratch = Path(tempfile.mkdtemp(prefix="reskate-oodle-proton-"))
    root = scratch / "Game root"
    root.mkdir()
    libraries = ["bcdec", "detours", "imgui", "lz4", "miniz", "Montserrat",
                 "PermanentMarker", "rapidjson", "spdlog", "sqlite", "steam_networking", "zstd"]
    malformed = []
    for library in libraries:
        path = root / ("licenses\\" + library + "-LICENSE.txt")
        path.write_text("Test filename only\n", encoding="utf-8")
        malformed.append(path)
    shutil.copy2(args.codec, root / "oo2core_9_win64.dll")
    environment = os.environ.copy()
    environment.update(WINEPREFIX=str(scratch / "prefix"), WINEARCH="win64", WINEDEBUG="-all",
                       WINEDLLOVERRIDES="mscoree,mshtml=")
    game_path = "Z:" + str(root).replace("/", "\\")
    results = []

    def run(label, executable):
        process = subprocess.run([str(args.wine.resolve()), str(executable.resolve()), game_path],
                                 env=environment, text=True, capture_output=True, timeout=120)
        row = {"test": label, "returncode": process.returncode,
               "stdout": process.stdout, "stderr": process.stderr}
        results.append(row)
        (scratch / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        return row

    print(f"Isolated fixture and results: {scratch}")
    original_bad = run("baseline with malformed names", args.baseline)
    if (original_bad["returncode"] == 0 or "Cannot load " not in original_bad["stderr"] or
            "licenses\\licenses" not in original_bad["stderr"]):
        parser.exit(1, "The original repeated-path failure was not reproduced; inspect results.json.\n")
    print("PASS: original loader reproduces the repeated-license-path error")
    fixed_bad = run("patched with malformed names", args.patched)
    if fixed_bad["returncode"] != 0 or "CAS byte decoded" not in fixed_bad["stdout"]:
        parser.exit(1, "Patched loader failed on malformed names; inspect results.json.\n")
    print("PASS: patched loader loads the root fixture and decodes with malformed names present")
    (root / "licenses").mkdir()
    for path in malformed:
        path.rename(root / "licenses" / path.name.split("\\", 1)[1])
    for label, executable in [("baseline with normalized names", args.baseline),
                              ("patched with normalized names", args.patched)]:
        result = run(label, executable)
        if result["returncode"] != 0 or "CAS byte decoded" not in result["stdout"]:
            parser.exit(1, f"{label} failed; inspect results.json.\n")
        print(f"PASS: {label}")
    print("No installed game, real Oodle DLL, mod, save, setting, or existing Wine prefix was used or changed.")


if __name__ == "__main__":
    main()
