"""Read-only supported-build contract check. Does not copy or install game data."""
import argparse
from pathlib import Path
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_exe", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = root / "Engine/Game/Build/20260929"
    data = args.game_exe.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("Not a PE executable")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    if machine != 0x8664:
        raise SystemExit("Expected x64")
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = pe + 24 + optional_size

    def read(rva, size):
        for i in range(count):
            at = sections + 40 * i
            virtual_size, start, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
            if start <= rva < start + max(virtual_size, raw_size):
                if rva + size > start + raw_size:
                    raise ValueError("RVA is not file-backed")
                offset = raw + rva - start
                return data[offset:offset + size]
        raise ValueError("RVA is outside the executable")

    values, arrays = {}, {}
    for name in ("placement_physics.h", "placement_adapters.h"):
        source = (build / name).read_text()
        for declaration in re.findall(r"inline constexpr std::uintptr_t ([^;]+);", source):
            for key, value in re.findall(r"(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)", declaration):
                values[key] = int(value, 0)
        for key, body in re.findall(r"inline constexpr std::array<[^>]+>\s*(\w+)\s*\{([^}]+)\}", source):
            arrays[key] = [int(number, 0) for number in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]
    checked = 0
    for key, expected in arrays.items():
        if not key.endswith("_prefix"):
            continue
        function = key.removesuffix("_prefix")
        if read(values[function], len(expected)) != bytes(expected):
            raise SystemExit(f"REFUSED: {function} prefix differs")
        checked += 1
    calls = 0
    for name in ("moving_calls", "material_calls", "transform_calls"):
        for call in arrays[name]:
            instruction = read(call, 5)
            if instruction[0] != 0xE8:
                raise SystemExit(f"REFUSED: {name} call opcode differs")
            destination = call + 5 + struct.unpack_from("<i", instruction, 1)[0]
            if name != "transform_calls" and destination != values["motion_type"]:
                raise SystemExit(f"REFUSED: {name} motion call target differs")
            calls += 1
    print(f"Verified {checked} native prefixes and {calls} call sites; no files installed or extracted.")


if __name__ == "__main__":
    main()
