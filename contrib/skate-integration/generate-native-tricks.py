"""Generate the native ignored-observation catalog from extracted DingoScorable JSON.
Usage: python generate-native-tricks.py extracted/scorable-assets output.h
Game assets are not included in the source package.
"""
from pathlib import Path
import json
import sys

rows = {}
for source in Path(sys.argv[1]).rglob("*.json"):
    data = json.loads(source.read_text(encoding="utf-8-sig"))
    if data.get("root_type") != "DingoScorable":
        continue
    for value in data["instances"]:
        name, identity = value.get("ScorableName"), value.get("BaseTrickName")
        if not name or not identity:
            continue
        category = value["TrickCategory"]
        assert 0 <= category < 32
        previous, mask = rows.get(name, (identity, 0))
        assert previous == identity, f"Ambiguous native identity: {name}"
        rows[name] = identity, mask | (1 << category)
assert len(rows) == 351, "Supported scoring catalog changed"
text = """#pragma once
#include <array>
#include <string_view>
#include <cstdint>
namespace dingosdk::skate {
// Generated from supported DingoScorable.ScorableName, BaseTrickName and TrickCategory.
// The native ignored-trick membership compares ScorableName strings, not the
// asset-name hash stored in CompositeTrickRecord. No localized text is used.
struct NativeTrickMetadata {std::string_view name,identity;std::uint32_t categories;};
inline constexpr std::array native_tricks{
"""
for name, (identity, mask) in sorted(rows.items()):
    text += f'    NativeTrickMetadata{{"{name}","{identity}",{mask:#x}u}},\n'
text += "};\n}\n"
Path(sys.argv[2]).write_text(text, encoding="utf-8")
print(f"Verified {len(rows)} native scoring names")
