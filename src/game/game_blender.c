// blender_open: the console's way to tools/blender/open_models.py (docs/ITEM_PIPELINE.md "Open models in
// Blender"), the same tool the terminal runs. The script runs in the background (no console window, the
// game keeps running): it builds stale item scenes headless, assembles a combined scene for several models
// and starts the Blender window detached. Its output (builds, errors, "ready") comes into the console line
// by line as it arrives (blender_open_poll, every frame).
//
//   blender_open <id ...|all|list> [--kind K] [--rebuild] [--no-open]   the script's arguments ("list" = --list)
//   blender_open dry <args>                                               only log the command line (tests)
//   bl <args>                                                             the same, shorter
//
// Needs the source tree (tools/ is not mirrored next to the exe: platform_data_path finds it through
// DG_SOURCE_DIR) and Python 3: the py launcher, else python on PATH (python3 first off Windows).
#include "game_internal.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define BO_SCRIPT "tools/blender/open_models.py"
#define BO_MAX_RUNS 4
#define BO_MAX_ARGS 48

typedef struct BoRun {
    SDL_Process *proc;
    char line[512];   // the output line being gathered
    int len;
} BoRun;

static BoRun s_runs[BO_MAX_RUNS];

#ifdef _WIN32
static const char *const k_python[][2] = { { "py", "-3" }, { "python", NULL } };
#else
static const char *const k_python[][2] = { { "python3", NULL }, { "python", NULL } };
#endif

// args[] = python [-3] -u <script> <the command's arguments>, NULL-terminated.
static void bo_argv(const char **args, int py, const char *script, int argc, const char **argv) {
    int n = 0;
    args[n++] = k_python[py][0];
    if (k_python[py][1]) args[n++] = k_python[py][1];
    args[n++] = "-u";   // unbuffered: each line reaches the console as it is printed
    args[n++] = script;
    for (int i = 0; i < argc && n < BO_MAX_ARGS - 1; i++) args[n++] = i == 0 && strcmp(argv[i], "list") == 0 ? "--list" : argv[i];
    args[n] = NULL;
}

static void bo_cmdline(char *buf, size_t size, const char **args) {
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; args[i] && o < size; i++) {
        bool quote = strchr(args[i], ' ') != NULL;
        o += (size_t)snprintf(buf + o, size - o, "%s%s%s%s", i ? " " : "", quote ? "\"" : "", args[i], quote ? "\"" : "");
    }
}

static void cmd_blender_open(int argc, const char **argv) {
    bool dry = argc > 1 && strcmp(argv[1], "dry") == 0;
    int first = dry ? 2 : 1;
    if (argc <= first) {
        log_info("usage: blender_open <id ...|all|list> [--kind K] [--rebuild] [--no-open] | blender_open dry <args> (docs/ITEM_PIPELINE.md)");
        return;
    }
    char script[1024], line[2048];
    const char *args[BO_MAX_ARGS];
    platform_data_path(BO_SCRIPT, script, sizeof(script));
    bo_argv(args, 0, script, argc - first, argv + first);
    bo_cmdline(line, sizeof(line), args);
    if (dry) {
        log_info("blender_open (dry): %s", line);
        return;
    }
    if (!platform_file_exists(script)) {
        log_warn("blender_open: no %s (it needs the source tree)", BO_SCRIPT);
        return;
    }
    int slot = -1;
    for (int i = 0; i < BO_MAX_RUNS && slot < 0; i++)
        if (!s_runs[i].proc) slot = i;
    if (slot < 0) {
        log_warn("blender_open: %d runs are still going; try again when one ends", BO_MAX_RUNS);
        return;
    }
    for (int py = 0; py < ARRAY_COUNT(k_python); py++) {
        bo_argv(args, py, script, argc - first, argv + first);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);   // no console window, never waited for
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process *p = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (p) {
            bo_cmdline(line, sizeof(line), args);
            log_info("blender_open: %s", line);
            memset(&s_runs[slot], 0, sizeof(s_runs[slot]));
            s_runs[slot].proc = p;
            return;
        }
    }
    log_warn("blender_open: could not start Python 3 (%s): install it (the py launcher, or python on PATH)", SDL_GetError());
}

static void bo_flush(BoRun *r) {
    if (r->len > 0 && r->line[r->len - 1] == '\r') r->len--;
    r->line[r->len] = 0;
    const char *s = strncmp(r->line, "open_models: ", 13) == 0 ? r->line + 13 : r->line;   // one prefix is enough
    if (*s) log_info("blender_open: %s", s);
    r->len = 0;
}

void blender_open_poll(void) {
    for (int i = 0; i < BO_MAX_RUNS; i++) {
        BoRun *r = &s_runs[i];
        if (!r->proc) continue;
        SDL_IOStream *out = SDL_GetProcessOutput(r->proc);
        char buf[1024];
        size_t n;
        while (out && (n = SDL_ReadIO(out, buf, sizeof(buf))) > 0) {
            for (size_t k = 0; k < n; k++) {
                if (buf[k] == '\n' || r->len == (int)sizeof(r->line) - 1) bo_flush(r);
                if (buf[k] != '\n') r->line[r->len++] = buf[k];
            }
        }
        if (out && SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) continue;   // still running
        bo_flush(r);
        SDL_WaitProcess(r->proc, false, NULL);
        SDL_DestroyProcess(r->proc);   // the object only: Blender, started detached, stays open
        r->proc = NULL;
        log_info("blender_open: finished");
    }
}

void blender_open_register(void) {
    cmd_register("blender_open", cmd_blender_open,
                 "blender_open <id ...|all|list> [--kind K] [--rebuild] [--no-open] | dry <args>: open models in Blender (tools/blender/open_models.py)");
    cmd_register("bl", cmd_blender_open, "bl <id ...|all|list> [--kind K] [--rebuild] [--no-open]: blender_open, shorter (bl smg)");
}
