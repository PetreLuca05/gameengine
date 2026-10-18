// Crash handler: logs a symbolized stack trace on unhandled exceptions (Windows / DbgHelp), or on
// fatal signals (macOS / Linux: backtrace(), then the default action so macOS still writes its
// .ips crash report; docs/PLATFORM_MACOS.md).
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   // sigaction under strict C11 (glibc)
#endif
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
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    log_error("CRASH: exception 0x%08lx at %p (exe+0x%llx)", er->ExceptionCode, er->ExceptionAddress,
              (unsigned long long)((const char *)er->ExceptionAddress - (const char *)GetModuleHandleW(NULL)));
    // Access violations: what was touched (a near-null address means an unchecked failed allocation).
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        log_error("CRASH: %s of address %p", er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                  (void *)er->ExceptionInformation[1]);
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
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void on_fatal_signal(int sig, siginfo_t *info, void *uctx) {
    (void)uctx;
    log_error("crash: signal %d (%s) at address %p", sig, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "SIGABRT",
              info ? info->si_addr : NULL);
    void *frames[64];
    int n = backtrace(frames, 64);
    char **sym = backtrace_symbols(frames, n);
    for (int i = 0; i < n; i++) log_error("  #%d %s", i, sym ? sym[i] : "?");
    // the default action (core dump / the macOS crash reporter) with the same signal
    signal(sig, SIG_DFL);
    raise(sig);
}

void crash_install(void) {
    // A peer closing a socket mid-write (phone server, LAN) must not kill the game.
    signal(SIGPIPE, SIG_IGN);
    if (getenv("DG_NO_CRASH_HANDLER")) return;   // let a debugger / sanitizer see the raw signal
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}
#endif
