// .story parser (docs/STORY.md "Narrative script"). Pure; see story_script.h.
#include "story/story_script.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

float story_cue_duration(int chars) {
    float d = 0.4f + 0.06f * (float)chars;
    return d < 1.5f ? 1.5f : d;
}

static char *trim(char *a) {
    while (*a == ' ' || *a == '\t') a++;
    char *e = a + strlen(a);
    while (e > a && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return a;
}

// Splits the bracket metadata on '·' (U+00B7) or '|'. Writes field pointers in place.
static int split_meta(char *m, char **f, int max) {
    int n = 0;
    char *start = m;
    for (char *c = m;; c++) {
        bool dot = (u8)c[0] == 0xC2 && (u8)c[1] == 0xB7;
        if (*c == '|' || dot || !*c) {
            bool end = !*c;
            *c = 0;
            if (n < max) f[n++] = trim(start);
            if (end) break;
            if (dot) c++;
            start = c + 1;
        }
    }
    return n;
}

static bool is_filter(const char *s) {
    static const char *const filters[] = {"door", "phone", "radio", "tv", "wall", "intercom"};
    for (int i = 0; i < ARRAY_COUNT(filters); i++)
        if (!strcmp(s, filters[i])) return true;
    return false;
}

// "NAME: text" with an upper-case speaker (letters, spaces, . ' -). Returns the text or NULL.
static char *cue_split(char *line, char *speaker, int speaker_size) {
    char *colon = strchr(line, ':');
    if (!colon || colon == line || colon - line >= speaker_size) return NULL;
    bool upper = false;
    for (char *c = line; c < colon; c++) {
        if (isupper((u8)*c)) upper = true;
        else if (!(*c == ' ' || *c == '.' || *c == '\'' || *c == '-' || isdigit((u8)*c))) return NULL;
    }
    if (!upper) return NULL;
    memcpy(speaker, line, (size_t)(colon - line));
    speaker[colon - line] = 0;
    return trim(colon + 1);
}

void story_script_free(StoryScript *s) {
    free(s->e);
    free(s->cue);
    free(s->buf);
    memset(s, 0, sizeof(*s));
}

bool story_script_parse(StoryScript *s, const char *text, char *err, int err_size) {
    memset(s, 0, sizeof(*s));
    if (err_size > 0) err[0] = 0;
    size_t len = strlen(text);
    // Bodies are rebuilt into a second buffer (stripped '>' markers), so reserve twice the input.
    s->buf = malloc(len * 2 + 2);
    char *src = s->buf, *dst = s->buf + len + 1;
    memcpy(src, text, len + 1);
    int e_cap = 0, c_cap = 0;
    StoryEntry *cur = NULL;
    int line_no = 0;
    bool ok = true;
    for (char *line = src; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        line_no++;
        char *l = trim(line);
        if (l[0] == '#' && l[1] == ' ' && !s->n && !cur) {
            dg_strcpy(s->title, sizeof(s->title), trim(l + 2));
        } else if (l[0] == '#' && l[1] == '#' && l[2] == ' ') {
            if (cur) *dst++ = 0; // terminate the previous body
            if (s->n == e_cap) {
                e_cap = e_cap ? e_cap * 2 : 32;
                s->e = realloc(s->e, sizeof(StoryEntry) * (size_t)e_cap);
            }
            cur = &s->e[s->n++];
            memset(cur, 0, sizeof(*cur));
            cur->first_cue = s->cue_n;
            char *h = trim(l + 3);
            char *br = strchr(h, '[');
            if (br) {
                char *close = strrchr(br, ']');
                if (!close) {
                    snprintf(err, (size_t)err_size, "line %d: unterminated [ in heading", line_no);
                    ok = false;
                    break;
                }
                *close = 0;
                *br = 0;
                char *f[6];
                int fn = split_meta(br + 1, f, 6);
                if (fn > 0) dg_strcpy(cur->kind, sizeof(cur->kind), f[0]);
                for (int i = 1; i < fn; i++) {
                    char *v = f[i];
                    size_t vl = strlen(v);
                    if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
                        v[vl - 1] = 0;
                        dg_strcpy(cur->title, sizeof(cur->title), v + 1);
                    } else if (is_filter(v)) {
                        dg_strcpy(cur->filter, sizeof(cur->filter), v);
                    } else if (!cur->speaker[0]) {
                        dg_strcpy(cur->speaker, sizeof(cur->speaker), v);
                    }
                }
            }
            if (!cur->kind[0]) dg_strcpy(cur->kind, sizeof(cur->kind), "text");
            dg_strcpy(cur->id, sizeof(cur->id), trim(h));
            for (int i = 0; i < s->n - 1; i++)
                if (!strcmp(s->e[i].id, cur->id)) {
                    snprintf(err, (size_t)err_size, "line %d: id '%s' defined twice", line_no, cur->id);
                    ok = false;
                }
            if (!cur->id[0]) {
                snprintf(err, (size_t)err_size, "line %d: heading without an id", line_no);
                ok = false;
            }
            cur->body = dst;
            if (!ok) break;
        } else if (cur) {
            if (l[0] == '>') l = trim(l + 1);
            if (!*l) {
                // Leading blank lines are dropped; a run of blanks keeps one empty line (paragraph break).
                // Every body line ends in '\n', so dst[-1] is one unless the body is empty.
                if (dst != cur->body && !(dst - 2 >= cur->body && dst[-2] == '\n')) *dst++ = '\n';
            } else {
                char speaker[32];
                size_t ll = strlen(l);
                memcpy(dst, l, ll);
                dst += ll;
                *dst++ = '\n';
                char tmp[1024];
                dg_strcpy(tmp, sizeof(tmp), l);
                char *cue_text = cue_split(tmp, speaker, sizeof(speaker));
                if (cue_text) {
                    if (s->cue_n == c_cap) {
                        c_cap = c_cap ? c_cap * 2 : 64;
                        s->cue = realloc(s->cue, sizeof(StoryCue) * (size_t)c_cap);
                    }
                    StoryCue *c = &s->cue[s->cue_n++];
                    dg_strcpy(c->speaker, sizeof(c->speaker), speaker);
                    dg_strcpy(c->text, sizeof(c->text), cue_text);
                    c->dur = story_cue_duration((int)strlen(cue_text));
                    c->start = cur->duration;
                    cur->duration += c->dur;
                    cur->cue_n++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!ok) {
        story_script_free(s);
        return false;
    }
    if (cur) *dst = 0;
    // Trim the bodies' trailing newlines and cut the cue texts at their line ends.
    for (int i = 0; i < s->n; i++) {
        char *b = (char *)s->e[i].body;
        size_t bl = strlen(b);
        while (bl && b[bl - 1] == '\n') b[--bl] = 0;
    }
    return true;
}

const StoryEntry *story_script_find(const StoryScript *s, const char *id) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->e[i].id, id)) return &s->e[i];
    return NULL;
}

typedef struct Missing {
    char *out;
    int size, len, n;
} Missing;

static void need(Missing *m, const StoryScript *s, const char *id, const char *what) {
    if (!id[0] || story_script_find(s, id)) return;
    m->n++;
    if (m->len >= m->size - 1) return;
    int w = snprintf(m->out + m->len, (size_t)(m->size - m->len), "%s: '%s' not in the script\n", what, id);
    if (w > 0) m->len = DG_MIN(m->len + w, m->size - 1);
}

int story_script_lint(const StoryDef *d, const StoryScript *s, char *out, int out_size) {
    Missing m = {out, out_size, 0, 0};
    if (out_size > 0) out[0] = 0;
    for (int i = 0; i < d->obj_n; i++) need(&m, s, d->obj[i].text, d->obj[i].id);
    for (int i = 0; i < d->prompt_n; i++) need(&m, s, d->prompt[i].text, d->prompt[i].id);
    for (int i = 0; i < d->collect_n; i++) need(&m, s, d->collect[i].text, d->collect[i].id);
    for (int i = 0; i < d->act_n; i++) {
        const StoryAction *a = &d->act[i];
        if (a->type == SA_SUBTITLE) need(&m, s, a->arg, "subtitle");
        if (a->type == SA_TITLE) need(&m, s, a->arg, "title");
        if (a->type == SA_AUDIO && a->ival == SAK_VO) need(&m, s, a->arg, "vo");
    }
    return m.n;
}
