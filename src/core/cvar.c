#include "engine.h"
#include "cvar.h"
#include "log.h"
#include "platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CVARS 512
#define MAX_CMDS 256
#define MAX_WRITERS 8
#define CBUF_SIZE 16384
#define MAX_ARGS 32

typedef struct Cmd { char name[CVAR_NAME_MAX]; char desc[CVAR_STR_MAX]; CmdFn fn; } Cmd;

typedef struct CvarSystem {
    Cvar *cvars[MAX_CVARS];
    int cvar_count;
    Cmd cmds[MAX_CMDS];
    int cmd_count;
    ConfigWriterFn writers[MAX_WRITERS];
    int writer_count;
    char cbuf[CBUF_SIZE];
    int cbuf_len;
    int wait_frames;
    Cvar *sv_cheats;
} CvarSystem;
#define s_cv (*g_engine.cvars)


static void parse_value(Cvar *cv) {
    switch (cv->type) {
    case CVAR_BOOL:
        cv->i = (atoi(cv->str) != 0 || strcmp(cv->str, "true") == 0) ? 1 : 0;
        cv->f = (float)cv->i;
        break;
    case CVAR_INT:
        cv->i = atoi(cv->str);
        if (cv->has_range) cv->i = (int)DG_CLAMP(cv->i, (int)cv->min, (int)cv->max);
        cv->f = (float)cv->i;
        break;
    case CVAR_FLOAT:
        cv->f = (float)atof(cv->str);
        if (cv->has_range) cv->f = DG_CLAMP(cv->f, cv->min, cv->max);
        cv->i = (int)cv->f;
        break;
    case CVAR_STRING:
        cv->f = (float)atof(cv->str);
        cv->i = atoi(cv->str);
        break;
    }
    // Normalize the stored string for numeric types after clamping.
    if (cv->type == CVAR_INT || cv->type == CVAR_BOOL) snprintf(cv->str, sizeof(cv->str), "%d", cv->i);
    if (cv->type == CVAR_FLOAT) {
        snprintf(cv->str, sizeof(cv->str), "%g", cv->f);
    }
}

static void cmd_set(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: set <cvar> <value>"); return; }
    if (!cvar_set(argv[1], argv[2])) log_warn("unknown cvar '%s'", argv[1]);
}

static void cmd_cvarlist(int argc, const char **argv) {
    const char *filter = argc > 1 ? argv[1] : "";
    for (int i = 0; i < s_cv.cvar_count; i++) {
        Cvar *cv = s_cv.cvars[i];
        if (filter[0] && !strstr(cv->name, filter)) continue;
        log_info("  %-24s = %-10s %s", cv->name, cv->str, cv->desc);
    }
}

static void cmd_cmdlist(int argc, const char **argv) {
    for (int i = 0; i < s_cv.cmd_count; i++) log_info("  %-20s %s", s_cv.cmds[i].name, s_cv.cmds[i].desc);
}

static void cmd_echo(int argc, const char **argv) {
    char buf[512] = { 0 };
    for (int i = 1; i < argc; i++) {
        strncat(buf, argv[i], sizeof(buf) - strlen(buf) - 2);
        strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
    }
    log_info("%s", buf);
}

static void cmd_exec(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: exec <file>"); return; }
    if (!config_exec_file(argv[1])) log_warn("exec: could not read '%s'", argv[1]);
}

static void cmd_reset(int argc, const char **argv) {
    if (argc < 2) return;
    Cvar *cv = cvar_find(argv[1]);
    if (cv) cvar_reset(cv);
}

static void cmd_toggle(int argc, const char **argv) {
    if (argc < 2) return;
    Cvar *cv = cvar_find(argv[1]);
    if (cv) cvar_set_int(cv, cv->i ? 0 : 1);
}

void cvar_system_init(void) {
    g_engine.cvars = (CvarSystem *)calloc(1, sizeof(CvarSystem));
    cmd_register("set", cmd_set, "set a cvar");
    cmd_register("cvarlist", cmd_cvarlist, "list cvars [filter]");
    cmd_register("cmdlist", cmd_cmdlist, "list commands");
    cmd_register("echo", cmd_echo, "print text");
    cmd_register("exec", cmd_exec, "execute a config file");
    cmd_register("reset", cmd_reset, "reset cvar to default");
    cmd_register("toggle", cmd_toggle, "toggle a boolean cvar");
    s_cv.sv_cheats = cvar_reg_bool("sv_cheats", true, CVAR_SERVER, "allow cheat cvars (dev build default on)");
}

void cvar_system_shutdown(void) {
    for (int i = 0; i < s_cv.cvar_count; i++) free(s_cv.cvars[i]);
    free(g_engine.cvars);
    g_engine.cvars = NULL;
}

Cvar *cvar_find(const char *name) {
    for (int i = 0; i < s_cv.cvar_count; i++)
        if (SDL_strcasecmp(s_cv.cvars[i]->name, name) == 0) return s_cv.cvars[i];
    return NULL;
}

Cvar *cvar_reg(const char *name, const char *def, CvarType type, u32 flags, const char *desc) {
    Cvar *cv = cvar_find(name);
    if (cv) return cv;
    if (s_cv.cvar_count >= MAX_CVARS) { log_fatal("too many cvars"); return NULL; }
    cv = (Cvar *)calloc(1, sizeof(Cvar));
    dg_strcpy(cv->name, sizeof(cv->name), name);
    dg_strcpy(cv->desc, sizeof(cv->desc), desc ? desc : "");
    dg_strcpy(cv->str, sizeof(cv->str), def);
    dg_strcpy(cv->default_str, sizeof(cv->default_str), def);
    cv->type = type;
    cv->flags = flags;
    parse_value(cv);
    dg_strcpy(cv->default_str, sizeof(cv->default_str), cv->str);
    s_cv.cvars[s_cv.cvar_count++] = cv;
    return cv;
}

Cvar *cvar_reg_bool(const char *name, bool def, u32 flags, const char *desc) {
    return cvar_reg(name, def ? "1" : "0", CVAR_BOOL, flags, desc);
}

Cvar *cvar_reg_int(const char *name, int def, int min, int max, u32 flags, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", def);
    Cvar *cv = cvar_reg(name, buf, CVAR_INT, flags, desc);
    cv->has_range = true; cv->min = (float)min; cv->max = (float)max;
    parse_value(cv);
    return cv;
}

Cvar *cvar_reg_float(const char *name, float def, float min, float max, u32 flags, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", def);
    Cvar *cv = cvar_reg(name, buf, CVAR_FLOAT, flags, desc);
    cv->has_range = true; cv->min = min; cv->max = max;
    parse_value(cv);
    return cv;
}

Cvar *cvar_reg_string(const char *name, const char *def, u32 flags, const char *desc) {
    return cvar_reg(name, def, CVAR_STRING, flags, desc);
}

void cvar_set_cv(Cvar *cv, const char *value) {
    if (cv->flags & CVAR_READONLY) { log_warn("%s is read-only", cv->name); return; }
    if ((cv->flags & CVAR_CHEAT) && s_cv.sv_cheats && !s_cv.sv_cheats->i) { log_warn("%s is cheat protected", cv->name); return; }
    char old[CVAR_STR_MAX];
    dg_strcpy(old, sizeof(old), cv->str);
    dg_strcpy(cv->str, sizeof(cv->str), value);
    parse_value(cv);
    if (strcmp(old, cv->str) != 0) {
        cv->modified++;
        if (cv->flags & CVAR_NOTIFY) log_info("%s changed to %s", cv->name, cv->str);
        if (cv->on_change) cv->on_change(cv);
    }
}

bool cvar_set(const char *name, const char *value) {
    Cvar *cv = cvar_find(name);
    if (!cv) return false;
    cvar_set_cv(cv, value);
    return true;
}

void cvar_set_float(Cvar *cv, float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v);
    cvar_set_cv(cv, buf);
}

void cvar_set_int(Cvar *cv, int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    cvar_set_cv(cv, buf);
}

void cvar_reset(Cvar *cv) { cvar_set_cv(cv, cv->default_str); }
int cvar_count(void) { return s_cv.cvar_count; }
Cvar *cvar_at(int index) { return (index >= 0 && index < s_cv.cvar_count) ? s_cv.cvars[index] : NULL; }

void cmd_register(const char *name, CmdFn fn, const char *desc) {
    for (int i = 0; i < s_cv.cmd_count; i++)
        if (SDL_strcasecmp(s_cv.cmds[i].name, name) == 0) { s_cv.cmds[i].fn = fn; return; }
    if (s_cv.cmd_count >= MAX_CMDS) { log_fatal("too many commands"); return; }
    Cmd *c = &s_cv.cmds[s_cv.cmd_count++];
    dg_strcpy(c->name, sizeof(c->name), name);
    dg_strcpy(c->desc, sizeof(c->desc), desc ? desc : "");
    c->fn = fn;
}

bool cmd_exists(const char *name) {
    for (int i = 0; i < s_cv.cmd_count; i++)
        if (SDL_strcasecmp(s_cv.cmds[i].name, name) == 0) return true;
    return false;
}

static int tokenize(char *line, const char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (p[0] == '/' && p[1] == '/') break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

void cmd_execute_line(const char *line_in) {
    char line[1024];
    dg_strcpy(line, sizeof(line), line_in);
    const char *argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);
    if (argc == 0) return;
    for (int i = 0; i < s_cv.cmd_count; i++) {
        if (SDL_strcasecmp(s_cv.cmds[i].name, argv[0]) == 0) {
            s_cv.cmds[i].fn(argc, argv);
            return;
        }
    }
    Cvar *cv = cvar_find(argv[0]);
    if (cv) {
        if (argc == 1) log_info("%s = \"%s\" (default \"%s\") - %s", cv->name, cv->str, cv->default_str, cv->desc);
        else cvar_set_cv(cv, argv[1]);
        return;
    }
    log_warn("unknown command '%s'", argv[0]);
}

void cbuf_add(const char *text) {
    int n = (int)strlen(text);
    if (s_cv.cbuf_len + n + 2 >= CBUF_SIZE) { log_warn("command buffer overflow"); return; }
    memcpy(s_cv.cbuf + s_cv.cbuf_len, text, (size_t)n);
    s_cv.cbuf_len += n;
    s_cv.cbuf[s_cv.cbuf_len++] = '\n';
    s_cv.cbuf[s_cv.cbuf_len] = 0;
}

void cbuf_execute(void) {
    if (s_cv.wait_frames > 0) { s_cv.wait_frames--; return; }
    int guard = 0;
    while (s_cv.cbuf_len > 0 && guard++ < 1000) {
        // Extract one command up to ';' or newline (respecting quotes).
        int i = 0; bool quoted = false;
        for (; i < s_cv.cbuf_len; i++) {
            char c = s_cv.cbuf[i];
            if (c == '"') quoted = !quoted;
            if (!quoted && (c == ';' || c == '\n')) break;
        }
        char line[1024];
        int n = DG_MIN(i, (int)sizeof(line) - 1);
        memcpy(line, s_cv.cbuf, (size_t)n);
        line[n] = 0;
        int consumed = (i < s_cv.cbuf_len) ? i + 1 : i;
        memmove(s_cv.cbuf, s_cv.cbuf + consumed, (size_t)(s_cv.cbuf_len - consumed));
        s_cv.cbuf_len -= consumed;
        s_cv.cbuf[s_cv.cbuf_len] = 0;

        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "wait", 4) == 0 && (p[4] == 0 || p[4] == ' ')) {
            int frames = atoi(p + 4);
            s_cv.wait_frames = frames > 0 ? frames : 1;
            return;
        }
        cmd_execute_line(line);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char **)a, *(const char **)b); }

int cmd_complete(const char *prefix, const char **out, int max) {
    int n = 0;
    size_t len = strlen(prefix);
    for (int i = 0; i < s_cv.cmd_count && n < max; i++)
        if (SDL_strncasecmp(s_cv.cmds[i].name, prefix, len) == 0) out[n++] = s_cv.cmds[i].name;
    for (int i = 0; i < s_cv.cvar_count && n < max; i++)
        if (SDL_strncasecmp(s_cv.cvars[i]->name, prefix, len) == 0) out[n++] = s_cv.cvars[i]->name;
    qsort(out, (size_t)n, sizeof(out[0]), cmp_str);
    return n;
}

void config_add_writer(ConfigWriterFn fn) {
    if (s_cv.writer_count < MAX_WRITERS) s_cv.writers[s_cv.writer_count++] = fn;
}

bool config_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { log_warn("config: cannot write %s", path); return false; }
    fprintf(f, "// Deadground config - generated, edits are kept for known cvars\n");
    for (int i = 0; i < s_cv.cvar_count; i++) {
        Cvar *cv = s_cv.cvars[i];
        if (cv->flags & CVAR_ARCHIVE) fprintf(f, "set %s \"%s\"\n", cv->name, cv->str);
    }
    for (int i = 0; i < s_cv.writer_count; i++) s_cv.writers[i](f);
    fclose(f);
    return true;
}

bool config_exec_file(const char *path) {
    size_t size = 0;
    char *text = platform_read_file(path, &size);
    if (!text) return false;
    // Execute immediately (configs never use wait).
    char *line = text;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[l - 1] = 0;
        cmd_execute_line(line);
        line = next;
    }
    free(text);
    return true;
}
