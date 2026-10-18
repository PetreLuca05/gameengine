#include "engine.h"
#include "cvar.h"
#include "log.h"
#include "platform.h"
#include "profiler.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CVARS 768
#define CVAR_INDEX 2048           // hash index slots (power of two, > 2 x MAX_CVARS)
#define MAX_CMDS 1024
#define MAX_WRITERS 8
#define CBUF_SIZE (512 * 1024)   // long generated test scripts (tools/tests/weapon_sway.cfg)
#define MAX_ARGS 32
#ifndef DG_DEV_BUILD
#define DG_DEV_BUILD 1   // CMake option DG_DEV_BUILD; the unit tests are dev builds
#endif

typedef struct Cmd { char name[CVAR_NAME_MAX]; char desc[CVAR_STR_MAX]; CmdFn fn; } Cmd;

typedef struct CvarSystem {
    Cvar *cvars[MAX_CVARS];
    int cvar_count;
    u16 index[CVAR_INDEX];        // case-insensitive name hash -> cvar index + 1 (0 = empty); PF-0002
    Cmd cmds[MAX_CMDS];
    int cmd_count;
    ConfigWriterFn writers[MAX_WRITERS];
    int writer_count;
    char cbuf[CBUF_SIZE];
    int cbuf_len;
    int wait_frames;
    Cvar *sv_cheats;
    // Config persistence: saves only when something archived changed (autosave + exit).
    bool cfg_disabled;     // --test: never load or write the user's config
    bool cfg_dirty;
    double cfg_dirty_time;
    bool cfg_backed_up;
    // Classification locks (docs/SETTINGS_AUDIT.md).
    bool locks;
    CvarSource src;
    bool (*rule_gate)(void);
    int rejected;
    bool loading_config;
    int config_dropped;
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

// help [filter]: every command (alphabetical) with its description; a filter matches names and
// descriptions ("help weather", "help vehicle").
static int cmd_name_cmp(const void *a, const void *b) { return SDL_strcasecmp((*(const Cmd *const *)a)->name, (*(const Cmd *const *)b)->name); }
static void cmd_help(int argc, const char **argv) {
    const char *f = argc > 1 ? argv[1] : NULL;
    static const Cmd *list[MAX_CMDS];
    int n = 0;
    for (int i = 0; i < s_cv.cmd_count; i++) {
        const Cmd *c = &s_cv.cmds[i];
        if (f && !SDL_strcasestr(c->name, f) && !SDL_strcasestr(c->desc, f)) continue;
        list[n++] = c;
    }
    qsort(list, (size_t)n, sizeof(list[0]), cmd_name_cmp);
    if (f) log_info("help: %d commands matching '%s' (help without a filter lists all %d; cvarlist [filter] lists the settings)", n, f, s_cv.cmd_count);
    else log_info("help: %d commands (help <word> filters, cvarlist [filter] lists the settings)", n);
    for (int i = 0; i < n; i++) log_info("  %-22s %s", list[i]->name, list[i]->desc);
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
    cmd_register("help", cmd_help, "help [filter]: list commands (sorted, filtered by name / description)");
    cmd_register("echo", cmd_echo, "print text");
    cmd_register("exec", cmd_exec, "execute a config file");
    cmd_register("reset", cmd_reset, "reset cvar to default");
    cmd_register("toggle", cmd_toggle, "toggle a boolean cvar");
    s_cv.locks = !DG_DEV_BUILD;
    s_cv.sv_cheats = cvar_reg_bool("sv_cheats", DG_DEV_BUILD != 0, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "allow cheat cvars (host rule; on in dev builds, off in release builds)");
}

void cvar_system_shutdown(void) {
    for (int i = 0; i < s_cv.cvar_count; i++) free(s_cv.cvars[i]);
    free(g_engine.cvars);
    g_engine.cvars = NULL;
}

static u32 name_hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)tolower((u8)*s++); h *= 16777619u; }
    return h;
}

// cvar_find is called from per-frame code (114 call sites): a hash index instead of a case-insensitive
// scan over every cvar (docs/perf/FINDINGS.md PF-0002). Same result; perf_fix_disable PF-0002 scans.
static Cvar *find(const char *name) {
    if (prof_fix_enabled(PFX_CVAR_HASH)) {
        for (u32 h = name_hash(name) & (CVAR_INDEX - 1);; h = (h + 1) & (CVAR_INDEX - 1)) {
            int k = s_cv.index[h];
            if (!k) return NULL;
            if (SDL_strcasecmp(s_cv.cvars[k - 1]->name, name) == 0) return s_cv.cvars[k - 1];
        }
    }
    for (int i = 0; i < s_cv.cvar_count; i++)
        if (SDL_strcasecmp(s_cv.cvars[i]->name, name) == 0) return s_cv.cvars[i];
    return NULL;
}

Cvar *cvar_find(const char *name) {
    prof_count(PC_CVAR_FIND, 1);
    PROF_BEGIN("cvar_find");   // its own zone: the per-call cost is what PF-0002 changes
    Cvar *cv = find(name);
    PROF_END();
    return cv;
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
    u32 h = name_hash(cv->name) & (CVAR_INDEX - 1);
    while (s_cv.index[h]) h = (h + 1) & (CVAR_INDEX - 1);
    s_cv.index[h] = (u16)(s_cv.cvar_count + 1);
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

// ------------------------------------------------------------------ classification locks

bool cvar_dev_build(void) { return DG_DEV_BUILD != 0; }
void cvar_set_release_locks(bool on) { if (g_engine.cvars) s_cv.locks = on; }
bool cvar_release_locks(void) { return g_engine.cvars && s_cv.locks; }
CvarSource cvar_source_push(CvarSource src) {
    CvarSource prev = s_cv.src;
    s_cv.src = src;
    return prev;
}
void cvar_source_pop(CvarSource prev) { s_cv.src = prev; }
void cvar_set_rule_gate(bool (*fn)(void)) { s_cv.rule_gate = fn; }
int cvar_rejected_count(void) { return g_engine.cvars ? s_cv.rejected : 0; }

const char *cvar_class_name(u32 flags) {
    u32 c = flags & CVAR_CLASS_MASK;
    if (c == CVAR_EDITABLE) return "EDITABLE";
    if (c == CVAR_TIER) return "TIER";
    if (c == CVAR_LOCKED) return (flags & CVAR_RULE) ? "LOCKED+RULE" : "LOCKED";
    return "?";
}

bool cvar_writable(const Cvar *cv, CvarSource src) {
    if (src == CVAR_SRC_HOST || !s_cv.locks) return true;
    u32 c = cv->flags & CVAR_CLASS_MASK;
    if (c == CVAR_TIER) return src == CVAR_SRC_FIDELITY;
    if (c == CVAR_LOCKED) return (cv->flags & CVAR_RULE) && (!s_cv.rule_gate || s_cv.rule_gate());
    return true;   // EDITABLE (or unclassified: the tests' own cvars)
}

// Saved to config.cfg: archived player settings and the host's archived rules.
bool cvar_saved(const Cvar *cv) {
    if (!(cv->flags & CVAR_ARCHIVE)) return false;
    u32 c = cv->flags & CVAR_CLASS_MASK;
    return c != CVAR_TIER && (c != CVAR_LOCKED || (cv->flags & CVAR_RULE));
}

void cvar_set_cv(Cvar *cv, const char *value) {
    if (cv->flags & CVAR_READONLY) { log_warn("%s is read-only", cv->name); return; }
    if (s_cv.loading_config && (cv->flags & (CVAR_TIER | CVAR_LOCKED)) && !cvar_saved(cv)) { s_cv.config_dropped++; return; }
    if (!cvar_writable(cv, s_cv.src)) {
        s_cv.rejected++;
        log_warn("%s is %s (docs/SETTINGS_AUDIT.md): write \"%s\" rejected", cv->name,
                 (cv->flags & CVAR_TIER) ? "set by Engine Fidelity" : ((cv->flags & CVAR_RULE) ? "a server rule of the host" : "fixed for fair play"), value);
        return;
    }
    if ((cv->flags & CVAR_CHEAT) && s_cv.sv_cheats && !s_cv.sv_cheats->i) { log_warn("%s is cheat protected", cv->name); return; }
    char old[CVAR_STR_MAX];
    dg_strcpy(old, sizeof(old), cv->str);
    dg_strcpy(cv->str, sizeof(cv->str), value);
    parse_value(cv);
    if (cv->bound) cv->bound(cv);
    if (strcmp(old, cv->str) != 0) {
        cv->modified++;
        if (cvar_saved(cv)) config_mark_dirty();
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
void cvar_store(Cvar *cv, const char *value) {
    dg_strcpy(cv->str, sizeof(cv->str), value);
    parse_value(cv);
}
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

int cmd_count(void) { return s_cv.cmd_count; }
const char *cmd_name_at(int index) { return index >= 0 && index < s_cv.cmd_count ? s_cv.cmds[index].name : ""; }
const char *cmd_desc_at(int index) { return index >= 0 && index < s_cv.cmd_count ? s_cv.cmds[index].desc : ""; }
const char *cmd_desc(const char *name) {
    for (int i = 0; i < s_cv.cmd_count; i++) if (!SDL_strcasecmp(s_cv.cmds[i].name, name)) return s_cv.cmds[i].desc;
    return NULL;
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

void cbuf_retry(const char *line) {
    int n = (int)strlen(line);
    if (s_cv.cbuf_len + n + 2 >= CBUF_SIZE) { log_warn("command buffer overflow"); return; }
    memmove(s_cv.cbuf + n + 1, s_cv.cbuf, (size_t)s_cv.cbuf_len);
    memcpy(s_cv.cbuf, line, (size_t)n);
    s_cv.cbuf[n] = '\n';
    s_cv.cbuf_len += n + 1;
    s_cv.cbuf[s_cv.cbuf_len] = 0;
    s_cv.wait_frames = 1;
}

void cbuf_execute(void) {
    if (s_cv.wait_frames > 0) { s_cv.wait_frames--; return; }
    int guard = 0;
    while (s_cv.cbuf_len > 0 && guard++ < 1000) {
        // Extract one command up to ';' or newline (respecting quotes; a '//' comment runs to the
        // end of the line, so a ';' inside it doesn't split it).
        int i = 0; bool quoted = false, comment = false;
        for (; i < s_cv.cbuf_len; i++) {
            char c = s_cv.cbuf[i];
            if (c == '\n') break;
            if (comment) continue;
            if (c == '"') quoted = !quoted;
            if (!quoted && c == '/' && i + 1 < s_cv.cbuf_len && s_cv.cbuf[i + 1] == '/') { comment = true; continue; }
            if (!quoted && c == ';') break;
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
        if (s_cv.wait_frames > 0) return;   // the command retries (cbuf_retry)
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

void config_set_enabled(bool enabled) { s_cv.cfg_disabled = !enabled; }
bool config_enabled(void) { return !s_cv.cfg_disabled; }
void config_mark_dirty(void) {
    if (!s_cv.cfg_dirty) s_cv.cfg_dirty_time = platform_time();
    s_cv.cfg_dirty = true;
}
void config_clear_dirty(void) { s_cv.cfg_dirty = false; }

bool config_save_if_dirty(const char *path) { return s_cv.cfg_dirty ? config_save(path) : false; }

bool config_autosave(const char *path) {
    // One second after the last burst of changes (a slider drag is many changes).
    if (!s_cv.cfg_dirty || platform_time() - s_cv.cfg_dirty_time < 1.0) return false;
    return config_save(path);
}

bool config_save(const char *path) {
    if (s_cv.cfg_disabled) return false;
    // First write of the session keeps the previous file as config.cfg.bak.
    if (!s_cv.cfg_backed_up) {
        s_cv.cfg_backed_up = true;
        size_t n = 0;
        char *old = platform_read_file(path, &n);
        if (old) {
            char bak[600];
            snprintf(bak, sizeof(bak), "%s.bak", path);
            platform_write_file(bak, old, n);
            free(old);
        }
    }
    FILE *f = fopen(path, "w");
    if (!f) { log_warn("config: cannot write %s", path); return false; }
    fprintf(f, "// Deadground config - generated, edits are kept for known cvars\n");
    for (int i = 0; i < s_cv.cvar_count; i++) {
        Cvar *cv = s_cv.cvars[i];
        if (cvar_saved(cv)) fprintf(f, "set %s \"%s\"\n", cv->name, cv->str);
    }
    for (int i = 0; i < s_cv.writer_count; i++) s_cv.writers[i](f);
    fclose(f);
    s_cv.cfg_dirty = false;
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

bool config_load_file(const char *path) {
    s_cv.loading_config = true;
    s_cv.config_dropped = 0;
    bool ok = config_exec_file(path);
    s_cv.loading_config = false;
    if (s_cv.config_dropped)
        log_info("config: %d entries of graphics / fair-play settings that are no longer saved were dropped (docs/SETTINGS_AUDIT.md)", s_cv.config_dropped);
    return ok;
}

int config_load_dropped(void) { return s_cv.config_dropped; }
