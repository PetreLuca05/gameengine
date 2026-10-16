// Crash handler: logs a symbolized stack trace on unhandled exceptions (Windows / DbgHelp).
#include "crash.h"
#include "log.h"
#include "engine.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static void log_stack(HANDLE thread, CONTEXT ctx) {
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        DWORD64 addr = frame.AddrPC.Offset;
        if (!addr) break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        const char *name = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
        if (SymGetLineFromAddr64(proc, addr, &ldisp, &line)) log_error("  #%02d %s  (%s:%lu)", i, name, line.FileName, line.LineNumber);
        else log_error("  #%02d %s  [%p]", i, name, (void *)addr);
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    log_error("CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    log_stack(GetCurrentThread(), *ep->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Hang watchdog (env DG_WATCHDOG=<seconds>): when the main loop's frame index stops advancing for
// that long, the main thread's stack is logged (once per stall), for scripted tests that hang.
static HANDLE g_main_thread;
static int g_wd_secs;
static DWORD WINAPI watchdog_thread(LPVOID unused) {
    (void)unused;
    u64 last = ~0ull;
    int stalled = 0;
    for (;;) {
        Sleep(1000);
        u64 f = g_engine.frame_index;
        stalled = f == last ? stalled + 1 : 0;
        last = f;
        if (stalled != g_wd_secs) continue;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        SuspendThread(g_main_thread);
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_main_thread, &ctx)) {
            log_error("WATCHDOG: no frame for %d s (frame %llu); main thread:", g_wd_secs, (unsigned long long)f);
            log_stack(g_main_thread, ctx);
        }
        ResumeThread(g_main_thread);
    }
    return 0;
}

void crash_install(void) {
    SetUnhandledExceptionFilter(crash_filter);
    const char *wd = getenv("DG_WATCHDOG");
    if (wd && atoi(wd) > 0) {
        g_wd_secs = atoi(wd);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    }
}
#else
void crash_install(void) {}
#endif
