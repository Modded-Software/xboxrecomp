"""Self-check that the generated `ebp` local starts at a defined value.

ebp is the one callee-saved register declared as a function local rather than
reached through a global macro. It was declared without an initialiser, and a
function with a real "push ebp; mov ebp, esp" prologue reads it on its very
first statement -- PUSH32(esp, ebp) runs before "ebp = esp" gives it a value.

So the emitted function begins by pushing an indeterminate word. At -O0 that is
whatever the host stack held; from -O1 up it is poison the compiler may
propagate, and the word goes into the guest stack as a frame pointer that the
epilogue pops back and that frame walkers may follow.
"""

import unittest

from . import config
from .translator import FunctionTranslator


BASE = 0x00011000

# push ebp; mov ebp, esp; pop ebp; ret  -- a real frame.
PROLOGUE = bytes.fromhex("558BEC5DC3")
# mov eax, [ebp+8]; xor eax, eax; pop ebp; ret -- reads the caller's frame.
FRAMELESS = bytes.fromhex("8B450833C05DC3")

# Every global config._install writes, so one test cannot leak into another.
_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)


class EbpInitTest(unittest.TestCase):
    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def _translate(self, image):
        config._install(
            [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
            entry_point=BASE, kernel_thunk_addr=BASE, origin="ebp-init-test")
        db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                     "_addr": BASE, "size": len(image)}}
        return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

    def test_frame_function_declares_ebp_initialised(self):
        c = self._translate(PROLOGUE)
        self.assertIn("uint32_t ebp = 0;", c)
        # The pre-fix spelling. This is the whole bug: an uninitialised local.
        self.assertNotIn("uint32_t ebp;", c)

    def test_the_prologue_really_does_read_ebp_first(self):
        """Why the initialiser matters: the push precedes the assignment."""
        c = self._translate(PROLOGUE)
        push = c.index("PUSH32(esp, ebp)")
        assign = c.index("ebp = esp")
        decl = c.index("uint32_t ebp")
        self.assertLess(decl, push, c)
        self.assertLess(push, assign,
                        "push no longer precedes the assignment; this test's "
                        "premise needs rechecking:\n" + c)

    def test_frameless_function_still_inherits_the_caller_frame(self):
        """The initialiser must not displace the frameless inheritance."""
        c = self._translate(FRAMELESS)
        self.assertIn("uint32_t ebp = 0;", c)
        self.assertIn("ebp = g_ebp;", c)
        self.assertLess(c.index("uint32_t ebp = 0;"), c.index("ebp = g_ebp;"), c)

    def test_biased_frame_is_published_before_calls(self):
        image = bytes.fromhex("558d6c249083ec74e80000000083c4745dc3")
        code = self._translate(image)
        self.assertIn("prologue saves caller's frame", code)
        self.assertIn("g_ebp = ebp;", code)
        self.assertIn("g_seh_ebp = ebp;", code)
        self.assertIn("frame stays current across calls", code)
        self.assertLess(code.index("g_seh_ebp = ebp;"),
                        code.index("RECOMP_ABI_CALL"))

    def test_cpu_id_toggle_probe_has_balanced_flags_stack_operations(self):
        image = bytes.fromhex("539c5889c33500002000509d9c58539d39d87405"
                              "b8010000000fa25bc3")
        code = self._translate(image)
        self.assertNotIn("RECOMP_UNIMPL", code)
        self.assertEqual(code.count("PUSH32(esp, (g_eflags"), 2)
        self.assertEqual(code.count("POP32(esp, g_eflags);"), 2)
        self.assertIn("int _cf", code)
        self.assertIn("0x0383F9FFu", code)

    def test_pushfd_snapshots_comparison_and_popfd_restores_branch_flags(self):
        image = bytes.fromhex("39d89c9d78029090c3")
        code = self._translate(image)
        self.assertIn("g_eflags = (g_eflags & ~0x40u)", code)
        self.assertIn("g_eflags = (g_eflags & ~0x80u)", code)
        self.assertIn("if (((g_eflags & 0x80u) != 0))", code)
        self.assertLess(code.index("g_eflags = (g_eflags & ~0x40u)"),
                        code.index("PUSH32(esp, (g_eflags"))


if __name__ == "__main__":
    unittest.main()
