"""Verify the supported native skate path against an extracted game RES (not distributed).
Usage: python verify-native-skate.py path/to/clientskatethrowdown__oncompositetrickscoringevent.res
This is static asset verification, not an in-game acceptance test.
"""
from pathlib import Path
import hashlib
import struct
import sys

b = Path(sys.argv[1]).read_bytes()
def words(at, count=1):
    return struct.unpack_from("<" + "I" * count, b, at)
def instruction(pc, expected, operands=None):
    assert words(0x3b8 + pc)[0] == expected, f"instruction changed at {pc:#x}"
    if operands is not None:
        assert words(0x3b8 + pc + 12, len(operands)) == operands, f"operands changed at {pc:#x}"

assert words(0x20, 10) == (64,472,466,781,6,0,0,655414,5177355,2048)
assert struct.unpack_from("<H", b, 0x3c)[0] == 54
assert words(0x3b8)[0] == 0xc27
sites = [words(0xff0+i*8)[0] for i in range(54) if words(0xfec+i*8)[0] == 0xb0b4285a]
assert sites == [0x190,0x248,0x2cc], sites
instruction(0x190,0x1bc03,(2,0xe0,5,0,0,0x40,2,0x1b9))
instruction(0x248,0x27403,(2,0x70,2,0x174,0,0x60,2,0x1bb))
instruction(0x2cc,0x2f803,(2,0x78,2,0x174,0,0x78,2,0x1bc))
instruction(0x21c,0x24803,(2,0x80,0,0x58,0,0x178,2,0x174))
instruction(0x298,0x2cc04,(3,0,0,0x68,0,0x17c,0,0x70,2,0x78))
instruction(0x834,0x86003,(3,0,0,0xe8,0,0x1b4,2,0x1cd))
instruction(0x87c,0x8a803,(3,0,0,0xf0,0,0x1b8,2,0x1cf))
instruction(0x8cc,0x8f002,(2,0x1ce,2,0x1d0,2,0x1d1))
for pc, op in ((0x478,0x4d023),(0x4d0,0x4fc03),(0x510,0x56823),(0x568,0x59403),
               (0x834,0x86003),(0x860,0x87c01),(0x87c,0x8a803),(0x8a8,0x8cc02),
               (0x8cc,0x8f002),(0x8f0,0x90026)):
    instruction(pc,op)
print("PASS: actual native allowed-category call and producers; separate ignored/banned calls; later native area/sequence path")
print("RES SHA256:",hashlib.sha256(b).hexdigest())
print("Static source/asset verification only; live gameplay acceptance remains pending.")

# Follow the real shipped control flow between ignored membership and sequence
# evaluation. Native functions are modeled only for Boolean/field operations;
# this is an asset-level control-flow replay, not live game acceptance.
calls = dict((pc, h) for h, pc in (words(0xfec + i * 8, 2) for i in range(54)))

def prefix(ignored, allowed=True, banned=False, first_valid=False, inside=True):
    registers = {(0, 0x1d0): False, (0, 0x1d1): True,
                 (2, 0x1b9): ignored, (2, 0x1b6): False,
                 (2, 0x1b7): False, (2, 0x1b8): False, (0, 0x188): 0}
    pc, stored_area = 0x1bc, None
    trace = []
    def get(pair):
        return registers.get(tuple(pair), 0)
    def put(pair, value):
        registers[tuple(pair)] = value
    while pc != 0x568:
        trace.append(pc)
        assert len(trace) < 100, "Unexpected native loop"
        word = words(0x3b8 + pc)[0]
        following, op = word >> 8, word & 255
        if op in (0x1e, 0x20):
            args = words(0x3b8 + pc + 4, 4)
            put(args[2:4], get(args[:2]))
        elif op == 0x26:
            args = words(0x3b8 + pc + 4, 3)
            if not get(args[:2]):
                following = args[2]
        elif op in (1, 2, 3, 4):
            h = calls[pc]
            if h == 0x1b74206c:  # Not(Boolean)
                args = words(0x3b8 + pc + 12, 4)
                put(args[2:4], not get(args[:2]))
            elif h == 0x8eb2196d:  # And(Boolean, Boolean)
                args = words(0x3b8 + pc + 12, 6)
                put(args[4:6], bool(get(args[:2]) and get(args[2:4])))
            elif pc == 0x248:
                put((2, 0x1bb), banned)
            elif pc == 0x2cc:
                put((2, 0x1bc), allowed)
            elif pc == 0x3b0:
                put((2, 0x134), int(first_valid))
            elif pc == 0x3dc:
                put((2, 0x1c2), get((2, 0x134)) != 0)
            elif pc == 0x4d0:
                put((2, 0x1b4), inside)
            else:
                assert pc in (0x1e8, 0x21c, 0x298), f"Unknown native call {pc:#x}"
        elif op == 0x23:
            if pc == 0x478:
                first_valid = True
            elif pc == 0x510:
                stored_area = get((2, 0x1b4))
            else:
                raise AssertionError(f"Unknown native setter {pc:#x}")
        else:
            raise AssertionError(f"Unknown native opcode {op:#x} at {pc:#x}")
        pc = following
    return first_valid, stored_area, trace

for name in ("powerslide", "tricklessair", "pumping", "manual", "stall", "slappyenter", "slappyenter_stall", "ground grab", "unknown", "disabled flip", "disabled grab", "disabled grind", "disabled ground trick"):
    valid, area, path = prefix(True)
    assert not valid and area is None and 0x478 not in path, name
    # The next core observation can seed FirstValidTrick: no submitted outcome
    # is manually injected to rescue a consumed turn.
    valid, area, path = prefix(False, first_valid=valid)
    assert valid and area is True and 0x478 in path, name
for inside in (False, True):
    valid, area, path = prefix(False, inside=inside)
    assert valid and area == inside
    valid, area, path = prefix(True, first_valid=True, inside=not inside)
    assert valid and area is None  # Ignored continuation cannot replace qualification.
assert not prefix(False, allowed=False)[0]
assert not prefix(False, banned=True)[0]
# skate.11 classifies disabled categories at the earlier ignored stage. They
# cannot overwrite an already-qualified core or seed a failed native attempt.
for first in (False, True):
    valid, area, path = prefix(True, allowed=False, banned=True, first_valid=first)
    assert valid == first and area is None and 0x478 not in path
# Downstream first-valid and native submission gates remain untouched.
instruction(0x7c0, 0x7ec03, (3,0,0,0xe0,0,0x180,2,0x13c))
instruction(0x7ec, 0x81002, (2,0x150,0,0x188,2,0x1cc))
assert words(0x3b8 + 0x824, 4) == (0x83426,2,0x1cb,0xc14)
assert words(0x3b8 + 0x1d8, 4) == (0x1e826,2,0x1ba,0x31c)
print("PASS: actual native bytecode replay: ignored observations do not seed FirstValidTrick; later core observation does")
print("PASS: native first-valid/area capture is unchanged, including outside qualification and ignored continuation")
