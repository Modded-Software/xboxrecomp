import argparse
from collections import Counter
import ctypes
from ctypes import wintypes
import time


class ThreadEntry(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                ("thread", wintypes.DWORD), ("process", wintypes.DWORD),
                ("priority", wintypes.LONG), ("delta", wintypes.LONG),
                ("flags", wintypes.DWORD)]


class SymbolInfo(ctypes.Structure):
    _fields_ = [("size", wintypes.ULONG), ("type_index", wintypes.ULONG),
                ("reserved", ctypes.c_ulonglong * 2), ("index", wintypes.ULONG),
                ("symbol_size", wintypes.ULONG), ("module", ctypes.c_ulonglong),
                ("flags", wintypes.ULONG), ("value", ctypes.c_ulonglong),
                ("address", ctypes.c_ulonglong), ("register", wintypes.ULONG),
                ("scope", wintypes.ULONG), ("tag", wintypes.ULONG),
                ("name_length", wintypes.ULONG), ("max_name", wintypes.ULONG),
                ("name", ctypes.c_char * 1)]


def sample(pid, seconds, delay):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    symbols = ctypes.WinDLL("dbghelp", use_last_error=True)
    signatures = {
        "OpenProcess": (wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
        "OpenThread": (wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
        "CloseHandle": (wintypes.BOOL, [wintypes.HANDLE]),
        "WaitForSingleObject": (wintypes.DWORD, [wintypes.HANDLE, wintypes.DWORD]),
        "CreateToolhelp32Snapshot": (wintypes.HANDLE, [wintypes.DWORD, wintypes.DWORD]),
        "Thread32First": (wintypes.BOOL, [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]),
        "Thread32Next": (wintypes.BOOL, [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]),
        "SuspendThread": (wintypes.DWORD, [wintypes.HANDLE]),
        "ResumeThread": (wintypes.DWORD, [wintypes.HANDLE]),
        "GetThreadContext": (wintypes.BOOL, [wintypes.HANDLE, ctypes.c_void_p]),
        "GetThreadTimes": (wintypes.BOOL, [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(kernel, name)
        function.restype, function.argtypes = result, arguments
    symbols.SymInitialize.restype = wintypes.BOOL
    symbols.SymInitialize.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.BOOL]
    symbols.SymFromAddr.restype = wintypes.BOOL
    symbols.SymFromAddr.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong,
                                  ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SymbolInfo)]
    symbols.SymCleanup.argtypes = [wintypes.HANDLE]
    process = kernel.OpenProcess(0x100410, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    handles, samples, initial_cpu, final_cpu = {}, {}, {}, {}
    initialized = False
    try:
        if delay and kernel.WaitForSingleObject(process, int(delay * 1000)) == 0:
            raise RuntimeError("Process exited before sampling")
        if not symbols.SymInitialize(process, None, True):
            raise ctypes.WinError(ctypes.get_last_error())
        initialized = True
        context_storage = ctypes.create_string_buffer(1248)
        context_address = (ctypes.addressof(context_storage) + 15) & ~15
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            snapshot = kernel.CreateToolhelp32Snapshot(4, 0)
            if snapshot == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(entry)
            try:
                available = kernel.Thread32First(snapshot, ctypes.byref(entry))
                while available:
                    if entry.process == pid and entry.thread not in handles:
                        handle = kernel.OpenThread(0x4A, False, entry.thread)
                        if handle:
                            handles[entry.thread] = handle
                            samples[entry.thread] = Counter()
                    available = kernel.Thread32Next(snapshot, ctypes.byref(entry))
            finally:
                kernel.CloseHandle(snapshot)
            for thread, handle in handles.items():
                timestamps = [wintypes.FILETIME() for unused in range(4)]
                if kernel.GetThreadTimes(handle, *(ctypes.byref(stamp) for stamp in timestamps)):
                    cpu = sum((stamp.dwHighDateTime << 32) | stamp.dwLowDateTime for stamp in timestamps[2:])
                    initial_cpu.setdefault(thread, cpu)
                    final_cpu[thread] = cpu
                if kernel.SuspendThread(handle) == 0xFFFFFFFF:
                    continue
                try:
                    ctypes.c_uint32.from_address(context_address + 48).value = 0x100001
                    if kernel.GetThreadContext(handle, context_address):
                        samples[thread][ctypes.c_uint64.from_address(context_address + 248).value] += 1
                finally:
                    kernel.ResumeThread(handle)
            if kernel.WaitForSingleObject(process, 20) == 0:
                break
        for thread in sorted(samples, key=lambda item: final_cpu.get(item, 0) - initial_cpu.get(item, 0), reverse=True)[:4]:
            resolved = Counter()
            for address, count in samples[thread].items():
                storage = ctypes.create_string_buffer(ctypes.sizeof(SymbolInfo) + 1024)
                symbol = ctypes.cast(storage, ctypes.POINTER(SymbolInfo))
                symbol.contents.size = ctypes.sizeof(SymbolInfo)
                symbol.contents.max_name = 1024
                displacement = ctypes.c_ulonglong()
                name = f"0x{address:016X}"
                if symbols.SymFromAddr(process, address, ctypes.byref(displacement), symbol):
                    name = ctypes.string_at(ctypes.addressof(storage) + SymbolInfo.name.offset,
                                            symbol.contents.name_length).decode(errors="replace")
                resolved[name] += count
            total = sum(resolved.values())
            cpu_seconds = (final_cpu.get(thread, 0) - initial_cpu.get(thread, 0)) / 10000000
            print(f"Thread {thread}: {cpu_seconds:.3f} CPU seconds, {total} wall-time samples")
            for name, count in resolved.most_common(25):
                print(f"  {count:5} {count / max(1, total):6.1%} {name}")
    finally:
        for handle in handles.values():
            kernel.CloseHandle(handle)
        if initialized:
            symbols.SymCleanup(process)
        kernel.CloseHandle(process)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Sample x64 Windows host thread PCs using local PDB symbols.")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--delay", type=float, default=0)
    arguments = parser.parse_args()
    sample(arguments.pid, arguments.seconds, arguments.delay)