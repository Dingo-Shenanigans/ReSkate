#!/usr/bin/env python3
"""Build an arm64 launcher around user-supplied ReSkate and Apple graphics."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = Path(__file__).resolve().parent
ARCHIVES = {
    "WS12WineSikarugir11.0_1.tar.xz": (
        "https://github.com/Sikarugir-App/Engines/releases/download/v1.0/WS12WineSikarugir11.0_1.tar.xz",
        "67e29fb3d74f363af39c69ba11f9b13a79812c5db07cf4748672658e4a200a0e"),
    "Template-1.0.21.tar.xz": (
        "https://github.com/Sikarugir-App/Template/releases/download/v1.0/Template-1.0.21.tar.xz",
        "bbe996e4e4375318485953d0c7818b7b4b0a4dc1f13303bcc584f99f7602f78d"),
}

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def archive(cache, name):
    url, expected = ARCHIVES[name]
    target = cache / name
    if not target.exists():
        partial = cache / (name + ".partial")
        run("/usr/bin/curl", "--fail", "--location", "--retry", "2", "--output", partial, url)
        if digest(partial) != expected:
            raise ValueError("Archive checksum mismatch: " + name)
        partial.replace(target)
    if digest(target) != expected:
        raise ValueError("Archive checksum mismatch: " + name)
    return target

def copytree(source, destination):
    shutil.copytree(source, destination, symlinks=True,
                    ignore=shutil.ignore_patterns(".DS_Store"))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gptk", required=True, type=Path,
                        help="mounted or extracted Apple Evaluation environment 4.0 beta 2 (download from Apple)")
    parser.add_argument("--reskate", required=True, type=Path,
                        help="extracted ReSkate release or MSVC output containing EXE, DLL and licenses")
    parser.add_argument("--cache", type=Path, default=ROOT / "build/macos/downloads")
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos/ReSkate.app")
    parser.add_argument("--dmg", action="store_true", help="also create a drag-to-Applications disk image")
    args = parser.parse_args()
    args.gptk = args.gptk.expanduser().resolve()
    args.reskate = args.reskate.expanduser().resolve()
    args.cache = args.cache.expanduser().resolve()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("Build on an Apple Silicon Mac with Xcode Command Line Tools installed.")
    output = args.output.expanduser().absolute()
    if output.suffix != ".app":
        parser.error("--output must end in .app")
    if output.exists() or (args.dmg and output.with_suffix(".dmg").exists()):
        parser.error("Output already exists; choose another --output or move the old build first.")
    for name in ("ReSkateLauncher.exe", "ReSkate.dll", "LICENSE.txt", "licenses"):
        if not (args.reskate / name).exists():
            parser.error("Missing ReSkate release file: " + name)
    candidates = [p.parent.parent for p in args.gptk.rglob("libd3dshared.dylib")
                  if p.parent.name == "external" and (p.parent.parent / "wine").is_dir()]
    if len(candidates) != 1:
        parser.error("Could not identify one Apple redist/lib directory under --gptk.")
    graphics = candidates[0]
    notices = list(args.gptk.rglob("*.rtf"))
    if not any("license" in p.name.lower() for p in notices):
        parser.error("Use the mounted Apple volume so its license and notices are included.")
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", "-R", "=anchor apple",
        graphics / "external/D3DMetal.framework")
    args.cache.mkdir(parents=True, exist_ok=True)
    wine = archive(args.cache, "WS12WineSikarugir11.0_1.tar.xz")
    libraries = archive(args.cache, "Template-1.0.21.tar.xz")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reskate-mac-", dir=output.parent) as temporary:
        stage = Path(temporary)
        app = stage / "ReSkate.app"
        resources = app / "Contents/Resources"
        executable = app / "Contents/MacOS/ReSkate"
        executable.parent.mkdir(parents=True)
        resources.mkdir(parents=True)
        unpack = stage / "unpack"
        unpack.mkdir()
        # Only pinned, checksum-verified upstream archives reach tar.
        run("/usr/bin/tar", "-xf", wine, "-C", unpack)
        run("/usr/bin/tar", "-xf", libraries, "-C", unpack)
        runtime = resources / "runtime"
        runtime.mkdir()
        copytree(unpack / "wswine.bundle", runtime / "wine")
        native = unpack / "Template-1.0.21.app/Contents/Frameworks"
        (runtime / "libraries").mkdir()
        # The template also contains other renderers and launchers. Only the
        # native support libraries used by this Wine build belong in our app.
        for source in native.iterdir():
            target = runtime / "libraries" / source.name
            if source.name == "GStreamer.framework":
                copytree(source, target)
            elif source.suffix == ".dylib":
                shutil.copy2(source, target, follow_symlinks=False)
        copytree(graphics, runtime / "graphics")
        legal = resources / "Apple Notices"
        legal.mkdir()
        for notice in notices:
            target = legal / notice.relative_to(args.gptk)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(notice, target)
        payload = resources / "ReSkate"
        payload.mkdir()
        for name in ("ReSkateLauncher.exe", "ReSkate.dll", "LICENSE.txt", "licenses", "launcher.json"):
            source = args.reskate / name
            if not source.exists():
                continue
            if source.is_dir():
                copytree(source, payload / name)
            else:
                shutil.copy2(source, payload / name)
        shutil.copy2(SOURCES / "display.reg", resources / "display.reg")
        shutil.copy2(SOURCES / "README.md", resources / "Mac Setup.md")
        shutil.copy2(ROOT / "LICENSE", resources / "LICENSE")
        manifest = {"archives": {name: {"url": url, "sha256": sha} for name, (url, sha) in ARCHIVES.items()},
                    "reskate": {name: digest(payload / name) for name in ("ReSkateLauncher.exe", "ReSkate.dll")},
                    "graphics": {str(p.relative_to(graphics)): digest(p) for p in sorted(graphics.rglob("*"))
                                 if p.is_file() and not p.is_symlink()}}
        (resources / "build-sources.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Reuse the project's icon. No private artwork or game assets are needed.
        iconset = stage / "ReSkate.iconset"
        iconset.mkdir()
        png = stage / "icon.png"
        run("/usr/bin/sips", "-s", "format", "png", ROOT / "assets/launcher/icon.ico", "--out", png)
        for size in (16, 32, 128, 256):
            run("/usr/bin/sips", "-z", size, size, png, "--out", iconset / f"icon_{size}x{size}.png")
        run("/usr/bin/iconutil", "-c", "icns", iconset, "-o", resources / "ReSkate.icns")
        # Build with the installed SDK; the tested runtime requires macOS 27.
        run("/usr/bin/xcrun", "swiftc", "-O", "-warnings-as-errors", "-target", "arm64-apple-macos15.0",
            SOURCES / "Core.swift", SOURCES / "Launcher.swift", "-o", executable)
        info = {"CFBundleName": "ReSkate", "CFBundleDisplayName": "ReSkate",
                "CFBundleIdentifier": "org.reskate.macos", "CFBundleExecutable": "ReSkate",
                "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
                "LSMinimumSystemVersion": "27.0", "NSHighResolutionCapable": True,
                "CFBundleIconFile": "ReSkate.icns"}
        with (app / "Contents/Info.plist").open("wb") as f:
            plistlib.dump(info, f)
        run("/usr/bin/codesign", "--force", "--sign", "-", app)
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
        app.rename(output)
    print("Built " + str(output))
    if args.dmg:
        with tempfile.TemporaryDirectory(prefix="reskate-dmg-", dir=output.parent) as temporary:
            content = Path(temporary)
            copytree(output, content / output.name)
            (content / "Applications").symlink_to("/Applications")
            run("/usr/bin/hdiutil", "create", "-volname", "ReSkate", "-srcfolder", content,
                "-format", "UDZO", output.with_suffix(".dmg"))
        print("Disk image: " + str(output.with_suffix(".dmg")))

if __name__ == "__main__":
    main()
