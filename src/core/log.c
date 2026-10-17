#include "engine.h"
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

typedef struct LogState {
    FILE *file;
    LogLine ring[LOG_RING_LINES];
    int head, count;
    u32 revision;
    LogLevel min_level;
    SDL_Mutex *mutex;
} LogState;
#define s_log (*g_engine.log)

static const char *k_level_names[] = { "debug", "info", "warn", "error", "FATAL" };

void log_init(const char *file_path) {
    g_engine.log = (LogState *)calloc(1, sizeof(LogState));
    s_log.mutex = SDL_CreateMutex();
    s_log.min_level = LOG_DEBUG;
    if (file_path) s_log.file = fopen(file_path, "w");
}

void log_shutdown(void) {
    if (!g_engine.log) return;
    if (s_log.file) fclose(s_log.file);
    s_log.file = NULL;
    if (s_log.mutex) SDL_DestroyMutex(s_log.mutex);
    free(g_engine.log);
    g_engine.log = NULL;
}

void log_set_min_level(LogLevel level) { if (g_engine.log) s_log.min_level = level; }

void log_msg(LogLevel level, const char *fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_engine.log) { fputs(buf, stderr); fputc('\n', stderr); if (level == LOG_FATAL) abort(); return; }
    if (level < s_log.min_level) return;

    if (s_log.mutex) SDL_LockMutex(s_log.mutex);
    double t = (double)SDL_GetTicks() / 1000.0;
    FILE *out = level >= LOG_WARN ? stderr : stdout;
    fprintf(out, "[%8.3f] %-5s %s\n", t, k_level_names[level], buf);
    if (s_log.file) {
        fprintf(s_log.file, "[%8.3f] %-5s %s\n", t, k_level_names[level], buf);
        fflush(s_log.file);
    }
    LogLine *line = &s_log.ring[s_log.head];
    line->level = level;
    dg_strcpy(line->text, sizeof(line->text), buf);
    s_log.head = (s_log.head + 1) % LOG_RING_LINES;
    if (s_log.count < LOG_RING_LINES) s_log.count++;
    s_log.revision++;
    if (s_log.mutex) SDL_UnlockMutex(s_log.mutex);

    if (level == LOG_FATAL) {
        // Scripted runs (--test, or DG_NO_FATAL_DIALOG set) abort without a dialog: nothing waits on the desktop.
        bool dialog = !SDL_getenv("DG_NO_FATAL_DIALOG");
        for (int i = 1; i < g_engine.argc && dialog; i++)
            if (!strcmp(g_engine.argv[i], "--test")) dialog = false;
        if (dialog) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Deadground fatal error", buf, NULL);
        abort();
    }
}

int log_line_count(void) { return g_engine.log ? s_log.count : 0; }

const LogLine *log_get_line(int index) {
    if (!g_engine.log || index < 0 || index >= s_log.count) return NULL;
    int start = (s_log.head - s_log.count + LOG_RING_LINES) % LOG_RING_LINES;
    return &s_log.ring[(start + index) % LOG_RING_LINES];
}

u32 log_revision(void) { return g_engine.log ? s_log.revision : 0; }
