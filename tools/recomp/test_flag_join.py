"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_are_not_guessed():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    assert _merge_flag_states([('cmp', wide), ('cmp', narrow)]) is None


def translate_movzx_join(source_imm=7):
    # movzx eax, word [esi+0x30]; cmp eax, imm; jmp join; nop;
    # cmp word [esi+0x30], 7; join: setne al; ret
    movzx = bytes.fromhex('0fb74630')
    if 0 <= source_imm < 0x80:
        cmp_wide = bytes.fromhex('83f8') + bytes([source_imm])
    else:
        cmp_wide = bytes.fromhex('81f8') + source_imm.to_bytes(4, 'little')
    prefix = movzx + cmp_wide
    jmp_at = len(prefix)
    direct = bytes.fromhex('66837e3007')  # 0x66 prefix: word, not dword
    join_at = jmp_at + 2 + len(direct)
    jmp = bytes([0xEB, join_at - (jmp_at + 2)])
    body = prefix + jmp + direct + bytes.fromhex('0f95c0c3')
    config._install([config.Section('.text', BASE, len(body), 0, len(body), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='movzx-flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(body),
                 '_addr': BASE, 'size': len(body)}}
    return FunctionTranslator(body, db).translate_function(BASE, db[BASE])


def test_movzx_narrow_compare_merges_with_direct_compare():
    # cmp eax, 7 where eax = zero-extended word [esi+0x30] is the same compare
    # as cmp word [esi+0x30], 7. Both must snapshot at 16 bits so the join
    # keeps a real condition instead of falling back to the never-taken _flags.
    code = translate_movzx_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code


def test_movzx_compare_with_wide_immediate_is_not_narrowed():
    # 0x10007 does not fit 16 bits: comparing the zero-extended word against it
    # differs from comparing the low word alone, so narrowing would answer the
    # wrong way and must not happen (the join falls back to _flags instead).
    code = translate_movzx_join(source_imm=0x10007)
    assert 'CMP_NE(_fa, _fb)' not in code, code
    assert '_flags /* setne */' in code, code
