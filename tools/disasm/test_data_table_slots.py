"""A data pointer only gets the long-body probe when it sits in a table.

_pass_data_ptr_targets scans data sections for code pointers and gives each a
short 64-instruction body probe, because a lone constant such as 0x00080000
lands in the code range by accident. A pointer with another code pointer beside
it is a table slot -- a vtable, a dispatch table -- and those can name long
methods: Ghost's cCloakShader::RenderSetup is 341 instructions. Only slots get
the long walk, and only when the next detected function start is close enough
that the alias extent is vouched for.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402

CODE_VA = 0x00001000
CODE_SIZE = 0x1000


class _Insn:
    def __init__(self, addr, mnemonic="mov"):
        self.address = addr
        self.end_address = addr + 1
        self.mnemonic = mnemonic


class _Section:
    executable = True

    def __init__(self, name, va, size, data=b""):
        self.name = name
        self.virtual_addr = va
        self.virtual_size = size
        self._data = data

    def __len__(self):
        return self.virtual_size


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Image:
    def __init__(self, code, data):
        self.sections = [code, data]
        self._code = code
        self._data = data

    def get_section_at_va(self, addr):
        return self._code

    def get_section_data(self, sec):
        return sec._data


class _Engine:
    def __init__(self, long_targets, targets):
        self.instructions = {a: _Insn(a) for a in targets}
        self._long = set(long_targets)

    def probes_as_function_body(self, addr, max_insns=8192):
        # Simulate a body only the long walk can reach.
        return max_insns >= 8192 and addr in self._long


def _word(v):
    return v.to_bytes(4, "little")


def _detector(long_targets, targets, funcs):
    # data: lone pointer 0x1100, table {0x1250,0x1260}, table {0x1500,0x1510}
    data = (_word(0x1100) + _word(0xAAAAAAAA)
            + _word(0x1250) + _word(0x1260)
            + _word(0xBBBBBBBB)
            + _word(0x1500) + _word(0x1510)
            + _word(0xCCCCCCCC))
    code = _Section(".text", CODE_VA, CODE_SIZE)
    dsec = _Section(".rdata", 0x9000, len(data), data)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(long_targets, targets)
    det.image = _Image(code, dsec)
    det.functions = {f.start: f for f in funcs}
    det._alias_entries = {}
    return det, code


class DataTableSlotTest(unittest.TestCase):
    def test_table_slot_gets_the_long_probe(self):
        funcs = [_Func(0x1050, 0x1060), _Func(0x1400, 0x1410),
                 _Func(0x1E00, 0x1E10)]
        det, code = _detector(long_targets={0x1250, 0x1260},
                              targets={0x1100, 0x1250, 0x1260},
                              funcs=funcs)
        self.assertTrue(det._pass_data_ptr_targets([code]))
        self.assertIn(0x1250, det._alias_entries)

    def test_lone_pointer_keeps_the_short_cap(self):
        funcs = [_Func(0x1050, 0x1060), _Func(0x1400, 0x1410),
                 _Func(0x1E00, 0x1E10)]
        det, code = _detector(long_targets={0x1100, 0x1250, 0x1260},
                              targets={0x1100, 0x1250, 0x1260},
                              funcs=funcs)
        det._pass_data_ptr_targets([code])
        self.assertNotIn(0x1100, det._alias_entries)

    def test_far_next_start_rejects_the_long_alias(self):
        funcs = [_Func(0x1050, 0x1060), _Func(0x1400, 0x1410),
                 _Func(0x1E00, 0x1E10)]
        det, code = _detector(long_targets={0x1500, 0x1510},
                              targets={0x1500, 0x1510},
                              funcs=funcs)
        det._pass_data_ptr_targets([code])
        self.assertNotIn(0x1500, det._alias_entries)
        self.assertNotIn(0x1510, det._alias_entries)


if __name__ == "__main__":
    unittest.main()