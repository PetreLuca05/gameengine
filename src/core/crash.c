// Crash handler: logs a symbolized stack trace on unhandled exceptions (Windows / DbgHelp).
#include "crash.h"
#include "log.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    log_error("CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
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
    return EXCEPTION_EXECUTE_HANDLER;
}

void crash_install(void) { SetUnhandledExceptionFilter(crash_filter); }
#else
void crash_install(void) {}
#endif
