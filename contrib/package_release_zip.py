#!/usr/bin/env python3
"""Create or check a release ZIP with portable, forward-slash entry names."""
import argparse
from pathlib import Path
import shutil
import stat
import tempfile
import zipfile


def check_name(name):
    parts = name.rstrip("/").split("/")
    if "\\" in name or ":" in name or any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"Non-portable ZIP entry: {name!r}")


def check_archive(path):
    with zipfile.ZipFile(path) as archive:
        seen = set()
        for entry in archive.infolist():
            # ZipInfo normalizes backslashes on Windows; inspect the stored spelling.
            name = entry.orig_filename
            check_name(name)
            key = name.rstrip("/").casefold()
            if key in seen or stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError(f"Duplicate or symlink ZIP entry: {name!r}")
            seen.add(key)
        damaged = archive.testzip()
        if damaged is not None:
            raise ValueError(f"Damaged ZIP entry: {damaged!r}")


def package(source, output):
    source = Path(source).resolve(strict=True)
    output = Path(output).absolute()
    if not source.is_dir():
        raise ValueError("The staged release must be a directory")
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Archive already exists: {output}")
    if output.resolve().is_relative_to(source):
        raise ValueError("Write the archive outside the staged release directory")
    files = []
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Symlinks are not release files: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f"Not a regular release file: {path}")
        name = path.relative_to(source).as_posix()
        check_name(name)
        files.append((path, name))
    if not files:
        raise ValueError("The staged release is empty")
    # Validate the completed archive before exposing the requested output name.
    with tempfile.TemporaryDirectory(prefix="reskate-release-", dir=output.parent) as temporary:
        staged = Path(temporary) / "release.zip"
        with zipfile.ZipFile(staged, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path, name in files:
                archive.write(path, arcname=name)
        check_archive(staged)
        created = False
        try:
            with output.open("xb") as target, staged.open("rb") as ready:
                created = True
                shutil.copyfileobj(ready, target)
        except BaseException:
            if created:
                output.unlink()
            raise
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", metavar="PATH", help="STAGED_FOLDER OUTPUT.zip")
    parser.add_argument("--check", type=Path, metavar="ARCHIVE.zip", help="Validate an existing ZIP without changing it")
    args = parser.parse_args()
    try:
        if args.check is not None:
            if args.paths:
                parser.error("--check accepts no staging/output paths")
            check_archive(args.check)
            print(f"Portable release ZIP: {args.check}")
        else:
            if len(args.paths) != 2:
                parser.error("Provide STAGED_FOLDER OUTPUT.zip, or --check ARCHIVE.zip")
            count = package(*args.paths)
            print(f"Packaged {count} file(s): {args.paths[1]}")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Release ZIP refused: {error}\n")


if __name__ == "__main__":
    main()
