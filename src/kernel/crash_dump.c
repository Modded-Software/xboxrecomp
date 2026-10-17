/**
 * Crash dumps: a host minidump and, optionally, a raw image of guest memory.
 *
 * The [CRASH] line a fault handler prints says where the host faulted and what
 * the guest registers held. It cannot be opened in a debugger afterwards, and
 * it does not hold the guest objects whose contents explain the fault. These
 * do:
 *
 *   <base>.dmp       host minidump (threads, host stacks, exception context);
 *                    open with WinDbg, or `minidump_stackwalk`/lldb on Linux.
 *   <base>.guest.bin guest address space, file offset == guest VA, so a hex
 *                    viewer or a script reads [va] directly. Unmapped and
 *                    unreadable ranges are holes (sparse on the host FS).
 *
 * <base> is RECOMP_CRASH_DIR/crash-<pid>-<seq>, or, when unset, the runtime
 * log path with ".log" replaced, so dumps sit beside the log of the run that
 * produced them.
 *
 *   RECOMP_CRASH_DUMP=0      off
 *   RECOMP_CRASH_DUMP=1      (default) small minidump: stacks + referenced
 *                            memory, ~0.1-2 MB
 *   RECOMP_CRASH_DUMP=full   full-memory minidump (can be several GB: the
 *                            guest reservation is part of the process)
 *   RECOMP_CRASH_RAM=1       also write <base>.guest.bin (guest RAM: 64/128 MB
 *                            of data, sparse)
 *
 * One dump per process unless the call says otherwise: a crash handler that
 * fires again while unwinding would otherwise overwrite the first, real one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xbox_memory_layout.h"
#include "crash_dump.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>

static volatile LONG g_crash_dumped;
static volatile LONG g_dump_seq;

static void dump_base(char *out, size_t n)
{
    const char *dir = getenv("RECOMP_CRASH_DIR");
    const char *log = getenv("RECOMP_LOG");
    LONG seq = InterlockedIncrement(&g_dump_seq);

    if (dir && *dir) {
        CreateDirectoryA(dir, NULL);
        _snprintf(out, n, "%s\\crash-%lu-%ld", dir,
                  (unsigned long)GetCurrentProcessId(), seq);
    } else if (log && *log) {
        size_t len = strlen(log);
        if (len > 4 && _stricmp(log + len - 4, ".log") == 0)
            len -= 4;
        _snprintf(out, n, "%.*s.dump%ld", (int)len, log, seq);
    } else {
        _snprintf(out, n, "crash-%lu-%ld",
                  (unsigned long)GetCurrentProcessId(), seq);
    }
    out[n - 1] = 0;
}

static unsigned long long write_minidump(const char *path,
                                         PEXCEPTION_POINTERS ep, int full)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_TYPE type;
    LARGE_INTEGER size;
    BOOL ok;

    if (f == INVALID_HANDLE_VALUE)
        return 0;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    type = full ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData
                                  | MiniDumpWithThreadInfo)
                : (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory
                                  | MiniDumpWithThreadInfo
                                  | MiniDumpScanMemory);
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                           ep ? &mei : NULL, NULL, NULL);
    size.QuadPart = 0;
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    if (!ok) {
        fprintf(stderr, "[CRASHDUMP] MiniDumpWriteDump failed (error %lu)\n",
                (unsigned long)GetLastError());
        DeleteFileA(path);
        return 0;
    }
    return (unsigned long long)size.QuadPart;
}

/* Guest memory, file offset == guest VA. Region by region so an unreadable
 * page (the trapped null page, a guard, a hole in the map) is skipped rather
 * than faulting the dumper inside the crash handler. */
static unsigned long long write_guest_ram(const char *path)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryBase();
    size_t total = xbox_GetMappedSize();
    size_t off = 0;
    unsigned long long written = 0;
    HANDLE f;

    if (!base || !total)
        return 0;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    while (off < total) {
        MEMORY_BASIC_INFORMATION mbi;
        size_t len;
        DWORD p;

        if (!VirtualQuery(base + off, &mbi, sizeof mbi))
            break;
        len = (size_t)((uint8_t *)mbi.BaseAddress + mbi.RegionSize
                       - (base + off));
        if (off + len > total)
            len = total - off;
        p = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)
                && p != PAGE_NOACCESS && p != 0) {
            LARGE_INTEGER pos;
            size_t done = 0;
            pos.QuadPart = (LONGLONG)off;
            SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
            while (done < len) {
                DWORD chunk = (DWORD)((len - done) > (1u << 24)
                                      ? (1u << 24) : (len - done));
                DWORD w = 0;
                if (!WriteFile(f, base + off + done, chunk, &w, NULL) || !w)
                    break;
                done += w;
            }
            written += done;
        }
        off += len;
    }
    /* Extend to the full mapped size so offsets past the last region read
     * as holes rather than end-of-file. */
    {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)total;
        SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
        SetEndOfFile(f);
    }
    CloseHandle(f);
    return written;
}

int xbox_CrashDump(void *exception_pointers, const char *reason, int force)
{
    PEXCEPTION_POINTERS ep = (PEXCEPTION_POINTERS)exception_pointers;
    const char *mode = getenv("RECOMP_CRASH_DUMP");
    const char *ram = getenv("RECOMP_CRASH_RAM");
    char base[MAX_PATH], path[MAX_PATH + 16];
    unsigned long long bytes;
    int full;

    if (mode && (!strcmp(mode, "0") || !_stricmp(mode, "off")))
        return 0;
    if (!force && InterlockedExchange(&g_crash_dumped, 1))
        return 0;
    full = mode && !_stricmp(mode, "full");
    dump_base(base, sizeof base);

    _snprintf(path, sizeof path, "%s.dmp", base);
    path[sizeof path - 1] = 0;
    bytes = write_minidump(path, ep, full);
    if (bytes)
        fprintf(stderr, "[CRASHDUMP] %s: %s minidump %s (%llu bytes)\n",
                reason ? reason : "dump", full ? "full" : "small", path, bytes);

    if (ram && *ram && strcmp(ram, "0")) {
        _snprintf(path, sizeof path, "%s.guest.bin", base);
        path[sizeof path - 1] = 0;
        bytes = write_guest_ram(path);
        fprintf(stderr, "[CRASHDUMP] %s: guest memory %s (%llu bytes of data, "
                "file offset = guest VA)\n", reason ? reason : "dump", path,
                bytes);
    }
    fflush(stderr);
    return 1;
}

static DWORD WINAPI crash_test_thread(LPVOID p)
{
    unsigned secs = (unsigned)(uintptr_t)p;
    volatile uint32_t *bad;

    Sleep(secs * 1000u);
    fprintf(stderr, "[CRASHDUMP] RECOMP_CRASH_TEST: faulting on purpose\n");
    fflush(stderr);
    bad = (volatile uint32_t *)(uintptr_t)0x10;
    *bad = 0xDEADBEEFu;
    return 0;
}

void xbox_CrashTestStart(void)
{
    const char *s = getenv("RECOMP_CRASH_TEST");
    HANDLE h;

    if (!s || !*s || *s == '0')
        return;
    h = CreateThread(NULL, 0, crash_test_thread,
                     (LPVOID)(uintptr_t)(s ? (unsigned)atoi(s) : 5u), 0, NULL);
    if (h)
        CloseHandle(h);
}

#else  /* !_WIN32 */

int xbox_CrashDump(void *exception_pointers, const char *reason, int force)
{
    (void)exception_pointers; (void)reason; (void)force;
    return 0;
}

void xbox_CrashTestStart(void) { }

#endif
