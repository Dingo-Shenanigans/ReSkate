"""Fix the switch tags of recordings made for regular-footed skaters before the switch fix.

    python fix_regular_skater_tags.py --tricks "D:/skate/ReSkate.Tricks" --output "D:/skate/ReSkate.Tricks-fixed"

1. Run the command above. It writes a fixed copy and never changes ReSkate.Tricks.
2. Check fix-regular-skater-tags-report.txt in the copy.
3. Close the game and back up ReSkate.Tricks.
4. Replace ReSkate.Tricks with the copy.

Recordings in a folder of their own (Name/Name.json) are fixed as well as loose ones and those in
recorded. Pass --before with the time the fixed build was first started on an install where recordings
were made that day: the default cutoff is midnight, and anything recorded after it on an older build
would be left as it was.

Before the fix, a recording stored the game's left-foot-leads bit as its switch tag. For a
regular-footed skater that bit is the inverse of switch. The script turns the tag over in saved
animations in the copy and in its recorded folder when all of these are true:

- the source is a recording (render_capture)
- the stance is not goofy
- the file changed before the cutoff (default: the day after the fix)

The script refuses a folder that holds a report, because it fixed that folder before.
"""
import argparse
import json
import re
import shutil
from datetime import datetime
from pathlib import Path

DEFAULT_CUTOFF = datetime(2026, 10, 21)
REPORT_NAME = "fix-regular-skater-tags-report.txt"
SWITCH_TAG = re.compile(r'("switch_stance"\s*:\s*)(true|false)')


def needs_fix(path, cutoff):
    if datetime.fromtimestamp(path.stat().st_mtime) >= cutoff:
        return False
    try:
        animation = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    if not isinstance(animation, dict) or animation.get("format") != "fourth-animation-studio":
        return False
    source = animation.get("source")
    kind = source.get("kind") if isinstance(source, dict) else None
    return kind == "render_capture" and animation.get("stance") != "goofy" and isinstance(animation.get("switch_stance"), bool)


def saved_animations(tricks):
    """Every saved animation: loose in the tricks folder, in recorded, and in a folder of its own named after
    it (Name/Name.json), which is how the studio saves one now. A folder without its own .json, such as
    presets or library, is passed over."""
    loose = sorted(tricks.glob("*.json"))
    recorded = sorted((tricks / "recorded").glob("*.json"))
    own = sorted(d / (d.name + ".json") for d in tricks.iterdir() if d.is_dir() and (d / (d.name + ".json")).is_file())
    return loose + recorded + own


def turn_switch_tag_over(path):
    text = path.read_text(encoding="utf-8")
    flipped, count = SWITCH_TAG.subn(lambda m: m.group(1) + ("false" if m.group(2) == "true" else "true"), text)
    if count != 1:
        raise ValueError(f"{path}: expected one switch_stance tag, found {count}")
    path.write_text(flipped, encoding="utf-8")


def fix_regular_skater_tags(tricks, output, cutoff=DEFAULT_CUTOFF):
    tricks, output = Path(tricks), Path(output)
    if output.exists():
        raise ValueError("Choose a new staging folder")
    if (tricks / REPORT_NAME).exists():
        raise ValueError(f"{tricks} holds {REPORT_NAME}: this script fixed it before")
    changed = [path.relative_to(tricks) for path in saved_animations(tricks) if needs_fix(path, cutoff)]
    shutil.copytree(tricks, output)
    for relative in changed:
        turn_switch_tag_over(output / relative)
    lines = [f"Cutoff: {cutoff.isoformat()}", f"Changed {len(changed)} file(s):"] + [relative.as_posix() for relative in changed]
    (output / REPORT_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return changed


def main():
    parser = argparse.ArgumentParser(description="Fix the switch tags of older regular recordings.")
    parser.add_argument("--tricks", required=True, type=Path, help="the ReSkate.Tricks folder")
    parser.add_argument("--output", required=True, type=Path, help="a new staging folder")
    parser.add_argument("--before", type=datetime.fromisoformat, default=DEFAULT_CUTOFF,
                        help="change only files changed before this date (default %(default)s)")
    args = parser.parse_args()
    changed = fix_regular_skater_tags(args.tricks, args.output, args.before)
    print(f"Changed {len(changed)} file(s) in {args.output}. See {args.output / REPORT_NAME}.")


if __name__ == "__main__":
    main()
