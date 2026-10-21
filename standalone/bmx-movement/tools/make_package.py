"""Package the audited v0.3.0 module and curated session02 evidence.

Requires a completed VERIFICATION.json from audit_release.py. Source/tools,
configured test programs, the final demo, library, videos and compact receipts are
included. Game binaries/assets, observer DLLs, raw frames, JSONL and large
trajectory dumps are excluded. An existing distribution is never overwritten.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
from urllib.parse import unquote, urlsplit
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.3.0"
METADATA_LIMIT = 16 * 1024 * 1024
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".cs", ".py", ".ps1",
                   ".mjs", ".md", ".txt", ".json", ".cmake", ".def", ".cmd"}
VIDEO_SUFFIXES = {".avi", ".mp4", ".gif", ".webm"}
RAW_SUFFIXES = {".jpg", ".jpeg", ".bmp", ".jsonl", ".dll", ".pyd", ".pyc",
                ".pebin", ".pid", ".pdb", ".obj", ".o"}
RAW_NAMES = {"pose-tracks-native.json", "joint-tracks-native.json",
             "vehicle-physical-native.json", "vehicle-ik-native.json"}
RAW_PREFIXES = ("flick-joint-native",)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def re_test_program(name):
    return re.fullmatch(r"bmx_[A-Za-z0-9_]*tests\.exe", name) is not None


def verify_packaged_bindings(verification, files):
    """Freeze packaged evidence against its audit hashes, including videos."""
    selected = {path.resolve(): path for path in files.values()}

    def visit(value):
        if isinstance(value, dict):
            path, expected = value.get("path"), value.get("sha256")
            if isinstance(path, str) and isinstance(expected, str):
                resolved = Path(path).resolve()
                if resolved in selected:
                    require(digest(selected[resolved]) == expected,
                            "Packaged audit evidence changed: " + str(resolved))
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(verification)


def markdown_target(text, start):
    """Read one Markdown destination, including angle quotes/parentheses."""
    while start < len(text) and text[start].isspace():
        start += 1
    if start < len(text) and text[start] == "<":
        end = text.find(">", start + 1)
        require(end >= 0, "Unclosed angle-quoted Markdown link")
        return text[start + 1:end]
    end, depth = start, 0
    while end < len(text):
        char = text[end]
        if char == "\\" and end + 1 < len(text):
            end += 2
            continue
        if char.isspace() or char == ")" and depth == 0:
            break
        depth += (char == "(") - (char == ")")
        end += 1
    require(depth == 0, "Unbalanced Markdown destination")
    return text[start:end]


def verify_relative_markdown_links(files):
    """Check the three public release documents against selected ZIP entries."""
    checked = []
    for document in ("README.md", "REFERENCE.md", "COMPARISON.md"):
        require(document in files, "Release document is not selected: " + document)
        text = files[document].read_text(encoding="utf-8-sig")
        starts = [match.end() for match in re.finditer(r"!?\[[^\n]*?\]\(\s*", text)]
        starts += [match.end() for match in re.finditer(r"(?m)^ {0,3}\[[^\]\n]+\]:\s*", text)]
        for start in starts:
            target = markdown_target(text, start)
            target = re.sub(r"\\([\\`*_{}\[\]()#+\-.!<> ])", r"\1", target)
            uri = urlsplit(target)
            if not target or uri.scheme or uri.netloc or not uri.path or uri.path.startswith(("/", "\\")):
                continue
            relative = unquote(uri.path).replace("\\", "/")
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(document), relative))
            require(resolved != ".." and not resolved.startswith(("../", "/")),
                    "Markdown link escapes the archive: " + document + " -> " + target)
            folder = resolved.rstrip("/") + "/"
            require(resolved in files or resolved == "." and files
                    or any(name.startswith(folder) for name in files),
                    "Markdown link is absent from selected archive files: " + document + " -> " + target)
            checked.append({"document": document, "target": target, "archive_path": resolved})
    return checked


def build_file_list(verification):
    files = {}

    def add(path, archive_path=None, required=True):
        path = Path(path)
        if not path.is_file():
            require(not required, "Required package input missing: " + str(path))
            return
        require(not path.is_symlink(), "Refused symlink package input: " + str(path))
        relative = archive_path or path.relative_to(ROOT).as_posix()
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts,
                "Invalid archive path: " + relative)
        require(relative not in files or files[relative] == path, "Archive path collision: " + relative)
        files[relative] = path

    for name in ("CMakeLists.txt", "README.md", "REFERENCE.md", "COMPARISON.md",
                 "LICENSE", "source-manifest.json", "VERIFICATION.json", ".gitignore"):
        add(ROOT / name)
    for directory in ("include", "src", "tests", "demo", "tools", "reference"):
        for path in (ROOT / directory).rglob("*"):
            relative = path.relative_to(ROOT)
            if (not path.is_file() or any(p in {"bin", "build", "__pycache__", ".git", "node_modules"} for p in relative.parts)
                    or path.suffix.lower() not in SOURCE_SUFFIXES):
                continue
            require(path.name not in RAW_NAMES and not path.name.startswith(RAW_PREFIXES),
                    "Raw trajectory payload in package source tree: " + str(path))
            add(path)
    programs = ["bmx_demo.exe"] + [Path(test["executable"]["path"]).name
                                   for test in verification["tests"]]
    require(len(programs) == len(set(programs)), "Duplicate audited programs")
    for name in programs:
        require(name == "bmx_demo.exe" or re_test_program(name), "Unexpected audited executable")
        add(ROOT / "build" / name, "bin/" + name)
    add(ROOT / "build/libbmx_movement.a", "lib/libbmx_movement.a")
    add(ROOT / "demo/run_observed_demo.cmd", "bin/run_observed_demo.cmd")
    session = ROOT / verification["session"]
    require(session.resolve().is_relative_to((ROOT / "recordings").resolve()), "Audit session escaped recordings")
    run_names = {r["name"] for r in verification["healthy_native_runs"] + verification["final_demo_runs"]}
    run_names.add(verification["visual_only_quarantine"]["name"])
    derived_contact_sheets = {(ROOT / comparison["directory"] / name).resolve()
                              for comparison in verification["comparisons"]
                              for name in ("contact-sheet.jpg", "all-58-tricks.jpg")}

    def evidence_file(path):
        name = path.name
        suffix = path.suffix.lower()
        if path.resolve() in derived_contact_sheets:
            return path.stat().st_size <= METADATA_LIMIT
        if (suffix in RAW_SUFFIXES or name in RAW_NAMES or name.startswith(RAW_PREFIXES)
                or name.startswith(("diagnostic", "frame-", "start-gate", "next-capture"))
                or name.endswith((".arm.json", ".navigation.json", ".connected.json"))):
            return False
        if suffix in VIDEO_SUFFIXES:
            return True
        return suffix in {".json", ".csv", ".png", ".md", ".txt"} and path.stat().st_size <= METADATA_LIMIT

    for name in sorted(run_names):
        directory = session / name
        require(directory.parent == session, "Invalid audited run basename")
        for path in directory.iterdir():
            if name == verification["visual_only_quarantine"]["name"] and path.name == "telemetry.csv":
                continue  # V3.2 startup failed: carry the limitation/video receipt, not an apparent physics CSV.
            if path.is_file() and evidence_file(path):
                add(path)
        for suffix in (".driver.json", ".csv", ".csv.summary.json"):
            add(session / (name + suffix), required=suffix == ".driver.json")
        analysis = session / ("analysis-" + name.removeprefix("streets-"))
        if analysis.is_dir():
            for path in analysis.iterdir():
                if path.is_file() and evidence_file(path):
                    add(path)
    for name in ("coverage-complete-primary.json", "pose-independent-repeat-comparison.json",
                 "analysis-controls-v33.json", "analysis-air-v33.json", "headless-release-validation.json", "deployment.json"):
        add(session / name, required=name in {"coverage-complete-primary.json", "deployment.json"})
    suffixes = [re.search(r"-(v2[a-z]+)$", run["name"]) for run in verification["final_demo_runs"]]
    require(suffixes and all(suffixes) and len({match[1] for match in suffixes}) == 1,
            "Final demos need one common capture suffix")
    tag = suffixes[0][1]
    expected_reports = {"actual-capture-quality-" + tag + ".json",
                        "actual-controls-hop-audit-" + tag + ".json", "session-manifest.json"}
    supplementary = verification["supplementary_reports"]
    reports = supplementary["files"]
    require(supplementary["capture_tag"] == tag and len(reports) == 3
            and {Path(report["path"]).name for report in reports} == expected_reports,
            "Final supplementary reports were not audited for this capture suffix")
    for report in reports:
        path = Path(report["path"]).resolve(strict=True)
        require(path.parent == session.resolve() and evidence_file(path),
                "Supplementary report escaped the selected session or compact metadata policy")
        require(digest(path) == report["sha256"], "Audited supplementary report changed: " + path.name)
        add(path)
    # The canonical provider catalog was exported on this earlier boot; later
    # captures reuse its identity. Keep compact source metadata, not its raw run.
    for name in ("trick-catalog.json", "runtime-inventory.json", "observation-schema.json", "capture-summary.json"):
        add(session / "streets-suspension01" / name)
    for pattern in ("live-*.json", "coverage-demo-*.json", "headless-v2d-validation.json"):
        for path in session.glob(pattern):
            if path.is_file() and evidence_file(path):
                add(path)
    # Carry the final compact numeric inputs behind the headless report when
    # they are inside the audited session; raw streams and build files stay out.
    for source in verification["headless_release_validation"]["sources"]:
        path = Path(source["path"]).resolve(strict=True)
        require(digest(path) == source["sha256"], "Headless audited input changed: " + str(path))
        if path.is_relative_to(session.resolve()) and evidence_file(path):
            add(path)
    for comparison in verification["comparisons"]:
        directory = ROOT / comparison["directory"]
        require(directory.resolve().is_relative_to(session.resolve()), "Comparison escaped audited session")
        for path in directory.iterdir():
            if path.is_file() and evidence_file(path):
                add(path)
    consumer = verification["installed_consumer"]["receipt"]
    receipt = Path(consumer["path"])
    require(digest(receipt) == consumer["sha256"], "Installed consumer receipt changed")
    add(receipt, "evidence/installed-consumer.json")
    for relative in files:
        require(Path(relative).suffix.lower() not in RAW_SUFFIXES
                or files[relative].resolve() in derived_contact_sheets,
                "Forbidden raw/game/runtime artifact in package: " + relative)
        require(Path(relative).name not in RAW_NAMES and not Path(relative).name.startswith(RAW_PREFIXES),
                "Forbidden raw trajectory payload in package: " + relative)
    return files


def main(args):
    name = args.name
    require(Path(name).name == name and "/" not in name and "\\" not in name
            and name.endswith(".zip"), "--name must be one ZIP basename")
    verification = read(ROOT / "VERIFICATION.json")
    require(verification.get("schema", 0) >= 3 and verification.get("version") == VERSION
            and verification.get("completed") is True, "A completed v0.3.0 release audit is required")
    require(verification.get("local_install_restoration_and_game_identity_verified") is True,
            "Local game restoration was not audited")
    require(verification["current_native_catalog_coverage"]["available"] == 58
            and verification["current_native_catalog_coverage"]["empty"] == 6, "Catalog coverage differs")
    require(verification["visual_only_quarantine"]["physics_coverage_valid"] is False,
            "V3.2 fixed-channel gap was not quarantined")
    require(verification.get("flick_hop_provenance", {}).get("actuation_phases")
            == ["enter", "recover", "transition"], "Flick command provenance was not audited")
    for relative, expected in verification["verified_files"].items():
        path = ROOT / relative
        require(digest(path) == expected, "Audited input changed; rerun audit: " + relative)
    files = build_file_list(verification)
    verify_packaged_bindings(verification, files)
    markdown_links = verify_relative_markdown_links(files)
    destination = ROOT.parent / name
    require(not destination.exists(), "Refuse replacing an existing distribution: " + str(destination))
    output = ROOT.parent / (destination.stem + ".package.json")
    require(not output.exists(), "Refuse replacing a prior package receipt")
    temporary = destination.with_name(destination.name + "." + uuid.uuid4().hex + ".tmp")
    manifest = {"schema": 3, "package": "bmx-movement", "version": VERSION, "license": "GPL-3.0",
                "verification_sha256": digest(ROOT / "VERIFICATION.json"),
                "session": verification["session"],
                "relative_markdown_links": markdown_links,
                "limits": verification["limits"] + [
                    "The archive contains only independent module code and measured numeric tracks, not game binaries, meshes, textures or clips.",
                    "Raw source frames, JSONL and large trajectory dumps remain in the workspace and are excluded.",
                    "The prebuilt archive/programs target MinGW Windows x64; rebuild for another compiler/platform.",
                    "Fresh ZIP extraction/build/consumer checks are recorded separately after archive creation."
                ], "files": []}
    try:
        with zipfile.ZipFile(temporary, "x", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6, allowZip64=True) as archive:
            for relative, path in sorted(files.items()):
                before = path.stat()
                expected = digest(path)
                archive.write(path, "bmx-movement/" + relative)
                after = path.stat()
                require(before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns
                        and digest(path) == expected, "Package input changed while archiving: " + relative)
                manifest["files"].append({"path": relative, "bytes": after.st_size, "sha256": expected})
            archive.writestr("bmx-movement/package-manifest.json", json.dumps(manifest, indent=2) + "\n")
        with zipfile.ZipFile(temporary) as archive:
            require(archive.testzip() is None, "ZIP CRC check failed")
            require(len(archive.infolist()) == len(files) + 1, "Unexpected ZIP entry count")
            for entry in manifest["files"]:
                h = hashlib.sha256()
                size = 0
                with archive.open("bmx-movement/" + entry["path"]) as stream:
                    for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
                        h.update(block)
                        size += len(block)
                require(size == entry["bytes"] and h.hexdigest() == entry["sha256"],
                        "Archived bytes differ: " + entry["path"])
        require(not destination.exists(), "Distribution appeared during packaging")
        temporary.rename(destination)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    receipt = {"schema": 2, "version": VERSION, "archive": str(destination),
               "bytes": destination.stat().st_size, "sha256": digest(destination),
               "file_count": len(files) + 1, "all_archive_entries_verified": True,
               "verification_sha256": manifest["verification_sha256"],
               "raw_frames_jsonl_game_dlls_and_build_trees_excluded": True,
               "fresh_extracted_build_checked": False}
    output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="bmx-movement-0.3.0.zip")
    main(parser.parse_args())
