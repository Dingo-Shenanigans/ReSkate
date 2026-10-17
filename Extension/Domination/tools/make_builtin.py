"""Writes the parks built into ReSkate (Extension/Domination/domination_parks.h) from park files.

usage: python -I make_builtin.py <out.h> <park.json>...

Each park is embedded as its JSON, one spot or start per line (raw string pieces the compiler
joins), without the generator's bookkeeping fields (source, area). A park file of the same name
in a parks folder or a map mod is read before the built-in one, so it can be overridden.
"""
import json, os, sys


def lines_of(park):
    head = {k: v for k, v in park.items() if k not in ("spots", "starts", "source")}
    out = [json.dumps(head, separators=(",", ":"))[:-1] + ',"spots":[']
    spots = [{k: v for k, v in s.items() if k != "area"} for s in park["spots"]]
    for i, spot in enumerate(spots):
        out.append(json.dumps(spot, separators=(",", ":")) + ("," if i + 1 < len(spots) else ""))
    out.append('],"starts":[')
    starts = park.get("starts", [])
    for i, start in enumerate(starts):
        out.append(json.dumps(start, separators=(",", ":")) + ("," if i + 1 < len(starts) else ""))
    out.append("]}")
    return out


def main():
    out, files = sys.argv[1], sys.argv[2:]
    parts = []
    for path in files:
        park = json.load(open(path, encoding="utf-8"))
        pid = os.path.splitext(os.path.basename(path))[0]
        body = "\n".join('        R"park(' + line + ')park"' for line in lines_of(park))
        parts.append(f'    BuiltinPark{{"{pid}",\n{body}}}')
    text = f"""#pragma once
#include <array>
#include <string_view>

// Domination parks built into ReSkate, so a map's park works without any file. Generated from
// park files by tools/make_builtin.py; do not edit by hand. A park file of the same name in a
// parks folder or a map mod is read first and overrides it.
namespace dingosdk::domination {{
struct BuiltinPark {{
    std::string_view id;   // as a park file's name without .json
    std::string_view json; // the park file (domination_model.h for the format)
}};
inline constexpr std::array<BuiltinPark, {len(parts)}> builtin_parks{{{{
{(",\n").join(parts)}}}}};
}}
"""
    open(out, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote", out, "with", len(parts), "park(s)")


if __name__ == "__main__":
    main()
