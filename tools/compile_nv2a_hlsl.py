import ctypes
import re
import sys
from pathlib import Path


def blob_method(blob, slot, result_type):
    table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(result_type, ctypes.c_void_p)(table[slot])


def compile_shader(compiler, source, entry, profile):
    encoded = source.encode("utf-8")
    buffer = ctypes.create_string_buffer(encoded)
    code = ctypes.c_void_p()
    errors = ctypes.c_void_p()
    result = compiler.D3DCompile(
        buffer, len(encoded), b"nv2a_gpu_shader.h", None, None,
        entry.encode("ascii"), profile.encode("ascii"),
        0x2000 | (4 if entry == "vs_main" else 0), 0,
        ctypes.byref(code), ctypes.byref(errors),
    )
    try:
        if result < 0:
            message = f"HLSL compiler HRESULT {result}"
            if errors:
                pointer = blob_method(errors, 3, ctypes.c_void_p)(errors)
                message = ctypes.string_at(pointer).decode("utf-8", errors="replace")
            raise RuntimeError(message)
    finally:
        for blob in (code, errors):
            if blob:
                blob_method(blob, 2, ctypes.c_ulong)(blob)


def main():
    if sys.platform != "win32":
        raise RuntimeError("Native shader compilation requires Windows D3DCompiler")
    compiler = ctypes.WinDLL("d3dcompiler_47.dll")
    compiler.D3DCompile.restype = ctypes.c_long
    compiler.D3DCompile.argtypes = (
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_uint, ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    )
    header = Path(__file__).resolve().parents[1] / "src/kernel/nv2a_gpu_shader.h"
    fragments = re.findall(r'R"HLSL\((.*?)\)HLSL"', header.read_text(encoding="utf-8"), re.DOTALL)
    if not fragments:
        raise RuntimeError("Embedded native HLSL not found")
    source = "".join(fragments)
    for entry, profile in (
        ("vs_main", "vs_5_0"), ("gs_main", "gs_5_0"),
        ("gs_lines_main", "gs_5_0"), ("gs_points_main", "gs_5_0"),
        ("ps_main", "ps_5_0"), ("ps_uncombined", "ps_5_0"),
    ):
        compile_shader(compiler, source, entry, profile)
        print(f"Compiled {entry} ({profile})", flush=True)
    prefix = "#define NV_W_DEPTH 1\n#define NV_DEPTH_SEMANTIC SV_DepthGreaterEqual\n"
    compile_shader(compiler, prefix + source, "ps_main", "ps_5_0")
    print("Compiled W-depth ps_main (ps_5_0)", flush=True)


if __name__ == "__main__":
    main()