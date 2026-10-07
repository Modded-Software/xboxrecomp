"""State-handler entries and the padding/jump-table boundary around them.

Ghost's cPlayerControl::GunCrouchStart (0x0007D4B0) is reached only through an
immediate stored into a runtime state table (mov eax, 0x7D4B0; ... store), so no
call and no data-section pointer names it. The linear sweep decoded its prologue
fine, but the function detector registered a *different* address 7 bytes later
(0x0007D4B7) and never the real one, so state 0x1C could not dispatch and the
title fell back to the previous state's handler.

Two defects stacked up, each with its own guard here:

* _pass_imm_ref_targets required the target to reach a `ret`, but a state
  handler may end in `jmp <other handler>`. A prologue plus a decodable body is
  enough in a gap.
* _pass_gap_prologues treated a lone `mov edi,edi` hot-patch pad in front of an
  out-of-line jump table as a function prologue, and that bogus start then
  measured across the table into the next real function. Rejecting the pad
  leaves the real entry (0x0007D4B0) detectable.
"""
import struct

from tools.disasm.test_decode_at import _engine, BASE
from tools.disasm.functions import FunctionDetector


def _populate(engine, data):
    engine.image.read_bytes_at_va = lambda addr, size: data[addr - BASE:addr - BASE + size]
    for insn in engine._cs.disasm(data, BASE):
        engine.instructions[insn.address] = engine._classify_instruction(insn)


def test_tail_jump_handler_reached_by_immediate_is_a_function():
    # mov eax, TARGET ; ret ; nop pad ; TARGET: sub esp,0x10 ; push ebx ; jmp self
    target_off = 16
    data = (b'\xb8' + struct.pack('<I', BASE + target_off) + b'\xc3'
            + b'\x90' * 10
            + bytes.fromhex('83ec10')            # sub esp, 0x10
            + bytes.fromhex('53')                # push ebx
            + bytes.fromhex('e900000000'))       # jmp $+5 (a tail jump, no ret)
    engine, section = _engine(BASE, data)
    _populate(engine, data)
    det = FunctionDetector(engine, engine.image, None, None)
    target = BASE + target_off
    assert engine.probes_as_prologue(target)
    assert engine.probes_as_function_body(target)
    assert not engine.probes_as_returning_body(target)   # no ret: the bug
    det._pass_imm_ref_targets([section])
    assert target in det._candidates


def test_gap_prologue_rejects_align_pad_before_a_jump_table():
    # ret 4 ; mov edi,edi ; <jump table right here>
    data = bytes.fromhex('c20400') + bytes.fromhex('8bff') + b'\x00' * 16
    engine, section = _engine(BASE, data)
    _populate(engine, data)
    det = FunctionDetector(engine, engine.image, None, None)
    nxt = BASE + 3
    assert engine.probes_as_prologue(nxt)                 # mov edi,edi matches
    engine.jump_tables[nxt + 2] = nxt + 2 + 8             # table in its window
    det._pass_gap_prologues([section])
    assert nxt not in det._candidates


if __name__ == "__main__":
    test_tail_jump_handler_reached_by_immediate_is_a_function()
    test_gap_prologue_rejects_align_pad_before_a_jump_table()
    print("ok")