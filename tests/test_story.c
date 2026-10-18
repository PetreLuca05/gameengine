// Unit tests for the pure story core (src/story: story_def, story_sim, story_script, story_save, story_bot;
// docs/STORY.md "Tests"): chapter parsing + reference errors, the lint (cycles, completion routes, manual
// objectives, end_chapter), the objective state machine (requires, groups, need N, optional, hidden,
// manual, roles, failure), triggers (arm, fallback, cooldown, `ever` events, timers, enable / disable),
// sequences with waits, the beat-windowed clock, the .story parser, save round trips mid-sequence, and
// the shipped chapters: lint clean, script complete, and the solver bot finishing every branch
// deterministically, with save / load at every checkpoint not changing the outcome.
#include "story/story.h"
#include "story/story_bot.h"
#include "story/story_save.h"
#include "story/story_script.h"
#include "story/vista_seq.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static StoryDef *def_new(const char *json) {
    StoryDef *d = malloc(sizeof(StoryDef));
    char err[256];
    if (!story_def_parse(d, json, err, sizeof(err))) {
        printf("parse error: %s\n", err);
        free(d);
        return NULL;
    }
    return d;
}

static StoryObjState ost(const StoryState *s, const StoryDef *d, const char *id) {
    return (StoryObjState)s->obj[story_find_objective(d, id)].state;
}

static void run(StoryState *s, const StoryDef *d, const StoryView *v, float seconds) {
    for (int i = 0; i < (int)lrintf(seconds * 10.0f); i++) story_tick(s, d, v, 0.1f, NULL);
}

static bool has_note(const StoryOut *o, StoryNoteKind kind, int ref) {
    for (int i = 0; i < o->count; i++)
        if (o->n[i].kind == kind && o->n[i].ref == ref) return true;
    return false;
}

// ------------------------------------------------------------------ parsing

static void test_clock_parse(void) {
    CHECK(story_parse_clock("05:52") == 352.0f, "05:52");
    CHECK(story_parse_clock("0:00") == 0.0f, "midnight");
    CHECK(story_parse_clock("90") == 90.0f, "plain minutes");
    CHECK(story_parse_clock("7:61") < 0 && story_parse_clock("x") < 0 && story_parse_clock("") < 0 && story_parse_clock("6:00am") < 0, "bad clocks");
}

static void test_parse_errors(void) {
    StoryDef *d = malloc(sizeof(StoryDef));
    char err[256];
    CHECK(!story_def_parse(d, "{nope", err, sizeof(err)) && strstr(err, "syntax"), "syntax error: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\"}", err, sizeof(err)) && strstr(err, "objectives"), "no objectives: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"},{\"id\":\"a\"}]}", err, sizeof(err)) && strstr(err, "twice"), "duplicate: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\",\"requires\":\"zz\"}]}", err, sizeof(err)) && strstr(err, "unknown objective 'zz'"), "unknown requires: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":true,\"do\":[{\"explode\":1}]}]}", err, sizeof(err)) && strstr(err, "unknown action"), "unknown action: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":{\"wibble\":1}}]}", err, sizeof(err)) && strstr(err, "unknown condition"), "unknown condition: %s", err);
    // The leading key names the action; later keys are parameters even when they name another action.
    CHECK(story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":true,\"do\":[{\"fade\":\"in\",\"time\":3},{\"camhint\":\"p\",\"time\":2}]}]}", err, sizeof(err))
              && d->act[d->trig[0].first_action].type == SA_FADE && d->act[d->trig[0].first_action].num[0] == 3.0f && d->act[d->trig[0].first_action + 1].type == SA_CAMHINT,
          "fade / camhint with a time parameter: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":{\"time\":\"25:99\"}}]}", err, sizeof(err)) && strstr(err, "clock"), "bad clock: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":true,\"do\":[{\"checkpoint\":\"nope\"}]}]}", err, sizeof(err)) && strstr(err, "checkpoint"), "unknown checkpoint: %s", err);
    CHECK(!story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"}],\"triggers\":[{\"id\":\"t\",\"when\":{\"timer\":3,\"since\":\"ghost\"}}]}", err, sizeof(err)) && strstr(err, "ghost"), "unknown since: %s", err);
    // Forward references resolve (objectives / triggers declared later).
    CHECK(story_def_parse(d, "{\"id\":\"x\",\"objectives\":[{\"id\":\"a\",\"requires\":\"b\",\"complete_on\":{\"timer\":1,\"since\":\"t2\"}},{\"id\":\"b\"}],"
                             "\"triggers\":[{\"id\":\"t1\",\"when\":true,\"do\":[{\"enable\":\"t2\"}]},{\"id\":\"t2\",\"when\":true,\"disabled\":true}]}", err, sizeof(err)),
          "forward refs: %s", err);
    free(d);
}

static void test_lint(void) {
    char out[2048];
    StoryDef *d = def_new("{\"id\":\"x\",\"objectives\":[{\"id\":\"a\",\"requires\":\"b\",\"complete_on\":true},{\"id\":\"b\",\"requires\":\"a\",\"complete_on\":true}]}");
    CHECK(d && story_def_validate(d, out, sizeof(out)) >= 2 && strstr(out, "cycle") && strstr(out, "end_chapter"), "cycle + no end: %s", out);
    free(d);
    d = def_new("{\"id\":\"x\",\"objectives\":[{\"id\":\"a\"},{\"id\":\"b\",\"activate\":\"manual\",\"complete_on\":true},{\"id\":\"h\",\"hidden\":true,\"complete_on\":true}],"
                "\"triggers\":[{\"id\":\"e\",\"when\":true,\"do\":{\"end_chapter\":true}},{\"id\":\"r\",\"when\":true,\"once\":false}]}");
    int n = story_def_validate(d, out, sizeof(out));
    CHECK(n == 4 && strstr(out, "objective a: required but nothing completes") && strstr(out, "objective b: manual") && strstr(out, "objective h: hidden")
              && strstr(out, "trigger r: repeatable"), "lint (%d): %s", n, out);
    free(d);
    d = def_new("{\"id\":\"x\",\"objectives\":[{\"id\":\"g\",\"group\":{\"need\":3},\"children\":[{\"id\":\"c1\",\"optional\":true,\"complete_on\":true}]},"
                "{\"id\":\"p\",\"requires\":\"k\",\"complete_on\":true,\"children\":[{\"id\":\"k\",\"complete_on\":true}]}],"
                "\"triggers\":[{\"id\":\"e\",\"when\":true,\"do\":{\"end_chapter\":true}}]}");
    story_def_validate(d, out, sizeof(out));
    CHECK(strstr(out, "needs 3 of 1") && strstr(out, "requires its own descendant k"), "group / descendant lint: %s", out);
    free(d);
}

// ------------------------------------------------------------------ objectives

static const char *OBJ_CHAPTER =
    "{\"id\":\"t\",\"checkpoints\":[\"start\",\"cp\"],"
    "\"roles\":[{\"slot\":0,\"id\":\"host\"},{\"slot\":1,\"id\":\"kid\",\"replaces\":[\"n2\"]}],"
    "\"objectives\":["
    " {\"id\":\"a\",\"complete_on\":{\"interact\":\"switch\"}},"
    " {\"id\":\"notes\",\"requires\":\"a\",\"checkpoint\":\"cp\",\"children\":["
    "   {\"id\":\"n1\",\"complete_on\":{\"item\":\"n1\",\"on\":\"read\"}},"
    "   {\"id\":\"n2\",\"complete_on\":{\"item\":\"n2\",\"on\":\"read\"}},"
    "   {\"id\":\"opt\",\"optional\":true,\"hidden\":true,\"complete_on\":{\"signal\":\"opt\"}}]},"
    " {\"id\":\"pick\",\"requires\":\"notes\",\"group\":{\"need\":2},\"children\":["
    "   {\"id\":\"p1\",\"optional\":true,\"complete_on\":{\"item\":\"p1\"}},"
    "   {\"id\":\"p2\",\"optional\":true,\"complete_on\":{\"item\":\"p2\"}},"
    "   {\"id\":\"p3\",\"optional\":true,\"complete_on\":{\"item\":\"p3\"}}]},"
    " {\"id\":\"late\",\"activate\":\"manual\",\"complete_on\":{\"signal\":\"late\"},\"fail_on\":{\"signal\":\"oops\"}},"
    " {\"id\":\"fin\",\"requires\":[\"pick\",\"late\"],\"complete_on\":{\"signal\":\"fin\"}}],"
    "\"triggers\":[{\"id\":\"go_late\",\"when\":{\"objective\":\"pick\"},\"do\":[{\"wait\":2},{\"objective\":\"late\",\"set\":\"active\"}]},"
    " {\"id\":\"end\",\"when\":{\"objective\":\"fin\"},\"do\":{\"end_chapter\":true}}]}";

static void test_objectives(void) {
    StoryDef *d = def_new(OBJ_CHAPTER);
    if (!d) { CHECK(0, "objective chapter parses"); return; }
    char out[1024];
    CHECK(story_def_validate(d, out, sizeof(out)) == 0, "lint clean: %s", out);
    StoryState *s = malloc(sizeof(StoryState));
    StoryOut *o = malloc(sizeof(StoryOut));
    story_init(s, d, 1);
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "a") == SOS_ACTIVE && ost(s, d, "notes") == SOS_INACTIVE && has_note(o, SN_OBJECTIVE, story_find_objective(d, "a")), "root active, dependent waits");
    // Reading n1 before 'notes' is active still counts once it activates (`ever` on complete_on).
    story_event(s, SEV_ITEM_READ, "n1");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "n1") == SOS_INACTIVE, "n1 inactive while 'a' is open");
    story_event(s, SEV_INTERACT, "switch");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "a") == SOS_DONE && ost(s, d, "notes") == SOS_ACTIVE, "a done -> notes active");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "n1") == SOS_DONE, "the early read counted (ever)");
    int leaves[8];
    int n = story_active_leaves(s, d, leaves, 8);
    CHECK(n == 1 && leaves[0] == story_find_objective(d, "n2"), "HUD leaf: n2 (hidden optional not shown), got %d", n);
    story_event(s, SEV_ITEM_READ, "n2");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "notes") == SOS_DONE && has_note(o, SN_CHECKPOINT, story_find_checkpoint(d, "cp")) && s->checkpoint == 1, "group done (optional ignored) + checkpoint");
    CHECK(ost(s, d, "pick") == SOS_ACTIVE && ost(s, d, "p1") == SOS_ACTIVE, "next group active");
    n = story_active_leaves(s, d, leaves, 8);
    CHECK(n == 1 && leaves[0] == story_find_objective(d, "pick"), "need-N group of optionals shows as the leaf");
    story_event(s, SEV_ITEM_PICKUP, "p3");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "pick") == SOS_ACTIVE, "1 of 2");
    story_event(s, SEV_ITEM_PICKUP, "p1");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "pick") == SOS_DONE && ost(s, d, "p2") == SOS_ACTIVE, "2 of 2: done, the rest stay open");
    // 'late' is manual: not active although nothing blocks it, until go_late's sequence (2 s wait).
    run(s, d, NULL, 1.0f);
    CHECK(ost(s, d, "late") == SOS_INACTIVE, "manual waits");
    run(s, d, NULL, 1.1f);
    CHECK(ost(s, d, "late") == SOS_ACTIVE, "activated by the sequence after the wait");
    story_event(s, SEV_SIGNAL, "oops");
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(ost(s, d, "late") == SOS_FAILED && ost(s, d, "fin") == SOS_INACTIVE, "fail_on");
    story_force_objective(s, d, story_find_objective(d, "late"), SOO_COMPLETE, o);
    CHECK(ost(s, d, "late") == SOS_DONE && ost(s, d, "fin") == SOS_ACTIVE, "forced complete (dev) unblocks");
    story_event(s, SEV_SIGNAL, "fin");
    story_tick(s, d, NULL, 0.1f, o);
    story_tick(s, d, NULL, 0.1f, o);
    CHECK(s->ended && has_note(o, SN_END, 0), "end_chapter");
    // Roles: the kid's role replaces n2.
    story_init(s, d, 1);
    story_set_role(s, 1, true);
    story_event(s, SEV_INTERACT, "switch");
    run(s, d, NULL, 0.3f);
    story_event(s, SEV_ITEM_READ, "n1");
    run(s, d, NULL, 0.2f);
    int n2 = story_find_objective(d, "n2");
    CHECK(s->obj[n2].state == SOS_DONE && s->obj[n2].by_role && ost(s, d, "notes") == SOS_DONE, "role replaced n2");
    // Reveal.
    int opt = story_find_objective(d, "opt");
    CHECK(!s->obj[opt].revealed, "hidden");
    story_force_objective(s, d, opt, SOO_REVEAL, o);
    CHECK(s->obj[opt].revealed && has_note(o, SN_OBJECTIVE, opt), "revealed + noted");
    free(o);
    free(s);
    free(d);
}

// ------------------------------------------------------------------ triggers, sequences, clock

static bool inside_kitchen(void *user, const char *volume, bool all) {
    DG_UNUSED(all);
    return *(bool *)user && !strcmp(volume, "kitchen");
}

static void test_triggers(void) {
    StoryDef *d = def_new(
        "{\"id\":\"t\",\"objectives\":[{\"id\":\"a\",\"complete_on\":{\"signal\":\"a\"}},{\"id\":\"b\",\"activate\":\"manual\",\"complete_on\":{\"signal\":\"b\"}}],"
        "\"triggers\":["
        " {\"id\":\"fb\",\"arm\":{\"objective\":\"a\"},\"when\":{\"volume\":\"kitchen\"},\"fallback\":5,\"do\":[{\"flag\":\"fb\"},{\"wait\":1.5},{\"flag\":\"fb\",\"value\":2},{\"weather\":\"rain\",\"blend\":30}]},"
        " {\"id\":\"rep\",\"when\":{\"signal\":\"ping\"},\"once\":false,\"cooldown\":1,\"do\":[{\"flag\":\"pings\",\"value\":1}]},"
        " {\"id\":\"tm\",\"when\":{\"timer\":3,\"since\":\"a\"},\"do\":[{\"flag\":\"tm\"},{\"enable\":\"late\"}]},"
        " {\"id\":\"late\",\"disabled\":true,\"when\":true,\"do\":[{\"flag\":\"late\"},{\"objective\":\"b\",\"set\":\"active\"}]},"
        " {\"id\":\"now\",\"when\":{\"signal\":\"x\"},\"do\":[{\"flag\":\"now\"}]},"
        " {\"id\":\"rnd\",\"when\":{\"random\":0.5},\"do\":[{\"flag\":\"rnd\"}]},"
        " {\"id\":\"e\",\"when\":{\"objective\":\"b\"},\"do\":{\"end_chapter\":true}}]}");
    if (!d) { CHECK(0, "trigger chapter parses"); return; }
    StoryState *s = malloc(sizeof(StoryState));
    StoryOut *o = malloc(sizeof(StoryOut));
    bool in_kitchen = false;
    StoryView v = {&in_kitchen, inside_kitchen, NULL};
    story_init(s, d, 7);
    run(s, d, &v, 1.0f);
    // Plain events are this-tick only (no `ever` on authored triggers).
    story_event(s, SEV_SIGNAL, "x");
    story_tick(s, d, &v, 0.1f, o);
    CHECK(story_flag(s, "now") == 1, "event trigger fired");
    in_kitchen = true;
    run(s, d, &v, 0.5f);
    CHECK(story_flag(s, "fb") == 0, "not armed until 'a' is done");
    story_event(s, SEV_SIGNAL, "a");
    run(s, d, &v, 0.2f); // a done (tick 1), fb arms + fires (tick 2: in the kitchen)
    CHECK(story_flag(s, "fb") == 1, "armed + volume -> fired");
    // Its sequence waits 1.5 s before setting 2 and emitting weather.
    run(s, d, &v, 1.3f);
    CHECK(story_flag(s, "fb") == 1, "still waiting");
    bool weather = false;
    for (int i = 0; i < 5 && !weather; i++) {
        story_tick(s, d, &v, 0.1f, o);
        for (int k = 0; k < o->count; k++)
            if (o->n[k].kind == SN_ACTION && o->n[k].act->type == SA_WEATHER && !strcmp(o->n[k].act->arg, "rain") && o->n[k].act->num[0] == 30.0f) weather = true;
    }
    CHECK(story_flag(s, "fb") == 2 && weather, "sequence resumed after the wait, runtime action emitted");
    // Repeatable with cooldown.
    story_event(s, SEV_SIGNAL, "ping");
    story_tick(s, d, &v, 0.1f, o);
    story_event(s, SEV_SIGNAL, "ping");
    story_tick(s, d, &v, 0.1f, o);
    int rep = story_find_trigger(d, "rep");
    CHECK(s->trig[rep].fired == 1, "cooldown blocks the second ping");
    run(s, d, &v, 1.0f);
    story_event(s, SEV_SIGNAL, "ping");
    story_tick(s, d, &v, 0.1f, o);
    CHECK(s->trig[rep].fired == 2, "fires again after the cooldown");
    // Timer since objective 'a' done (3 s) enables 'late', which activates manual 'b' -> the end trigger waits on b.
    run(s, d, &v, 1.5f);
    CHECK(story_flag(s, "tm") == 1 && story_flag(s, "late") == 1 && ost(s, d, "b") == SOS_ACTIVE, "timer -> enable -> manual activation");
    story_event(s, SEV_SIGNAL, "b");
    run(s, d, &v, 0.2f);
    CHECK(s->ended, "ended");

    // Fallback without the condition: never in the kitchen, fires 5 s after arming.
    in_kitchen = false;
    story_init(s, d, 7);
    story_event(s, SEV_SIGNAL, "a");
    run(s, d, &v, 0.1f);
    int fb = story_find_trigger(d, "fb");
    run(s, d, &v, 0.1f);
    float armed = s->trig[fb].t_armed;
    run(s, d, &v, 4.7f);
    CHECK(s->trig[fb].fired == 0, "fallback not yet (%.2f s armed)", s->now - armed);
    run(s, d, &v, 0.4f); // 5.0 s of 0.1 s float steps may land a hair under 5: allow one tick
    CHECK(s->trig[fb].fired == 1 && fabsf(s->trig[fb].t_fired - armed - 5.0f) < 0.15f, "fallback after 5 s (%.2f)", s->trig[fb].t_fired - armed);
    // Random is seeded: same seed, same tick of firing.
    int rnd = story_find_trigger(d, "rnd");
    story_init(s, d, 99);
    run(s, d, &v, 2.0f);
    float t1 = s->trig[rnd].t_fired;
    story_init(s, d, 99);
    run(s, d, &v, 2.0f);
    CHECK(s->trig[rnd].fired && s->trig[rnd].t_fired == t1, "seeded random");
    free(o);
    free(s);
    free(d);
}

static void test_clock(void) {
    StoryDef *d = def_new(
        "{\"id\":\"c\",\"start\":{\"time\":\"05:50\"},\"objectives\":[{\"id\":\"a\",\"complete_on\":{\"signal\":\"a\"}},{\"id\":\"b\",\"requires\":\"a\",\"complete_on\":{\"signal\":\"b\"}}],"
        "\"timeline\":[{\"beat\":\"a\",\"from\":\"06:00\",\"to\":\"06:10\",\"expected\":100},{\"beat\":\"b\",\"from\":\"07:00\",\"to\":\"07:30\",\"expected\":300}],"
        "\"triggers\":[{\"id\":\"jump\",\"when\":{\"signal\":\"jump\"},\"do\":[{\"time\":\"08:00\",\"blend\":10}]},"
        " {\"id\":\"e\",\"when\":{\"signal\":\"end\"},\"do\":{\"end_chapter\":true}}]}");
    if (!d) { CHECK(0, "clock chapter parses"); return; }
    StoryState *s = malloc(sizeof(StoryState));
    story_init(s, d, 1);
    CHECK(s->clock == 350.0f, "start time");
    // Catch up 05:50 -> 06:00 at 2 min/s = 5 s, then 10 min over 100 s.
    run(s, d, NULL, 5.0f);
    CHECK(fabsf(s->clock - 360.0f) < 0.25f, "caught up to 06:00 (%.2f)", s->clock);
    run(s, d, NULL, 50.0f);
    CHECK(fabsf(s->clock - 365.0f) < 0.25f, "half-way through the window (%.2f)", s->clock);
    run(s, d, NULL, 200.0f);
    CHECK(s->clock == 370.0f, "holds at the window's end (%.2f)", s->clock);
    CHECK(story_current_beat(s, d) == 0, "beat a");
    story_event(s, SEV_SIGNAL, "a");
    run(s, d, NULL, 1.0f);
    CHECK(story_current_beat(s, d) == 1, "beat b");
    run(s, d, NULL, 25.0f); // 50 minutes to catch up at 2 min/s
    CHECK(fabsf(s->clock - 420.0f) < 0.25f, "caught up to 07:00 (%.2f)", s->clock);
    story_event(s, SEV_SIGNAL, "jump");
    run(s, d, NULL, 5.0f);
    CHECK(s->clock_override && s->clock > 440.0f && s->clock < 460.0f, "blending to 08:00 (%.2f)", s->clock);
    run(s, d, NULL, 5.2f);
    CHECK(!s->clock_override && s->clock == 480.0f, "blend done (%.2f)", s->clock);
    run(s, d, NULL, 10.0f);
    CHECK(s->clock == 480.0f, "past the window: holds");
    story_set_clock(s, 400.0f);
    CHECK(s->clock == 400.0f, "story_time scrub");
    free(s);
    free(d);
}

// ------------------------------------------------------------------ script

static void test_script(void) {
    const char *text =
        "# 99 · Test\n"
        "<!-- comment -->\n"
        "## obj.a  [objective]\n"
        "Do the thing.\n"
        "\n"
        "## doc.n  [document · \"A note\"]\n"
        "> line one\n"
        "> line two\n"
        "\n"
        "\n"
        "> after a gap\n"
        "\n"
        "## vo.k  [voice · Ruth · door]\n"
        "RUTH: Hello? It's Ruth.\n"
        "MR. O'NEIL: Short.\n"
        "Not a cue: lower case speaker? no, this is: a plain line\n";
    StoryScript sc;
    char err[128];
    CHECK(story_script_parse(&sc, text, err, sizeof(err)), "parse: %s", err);
    CHECK(!strcmp(sc.title, "99 · Test") && sc.n == 3, "title + 3 entries (%d)", sc.n);
    const StoryEntry *a = story_script_find(&sc, "obj.a"), *n = story_script_find(&sc, "doc.n"), *k = story_script_find(&sc, "vo.k");
    CHECK(a && !strcmp(a->kind, "objective") && !strcmp(a->body, "Do the thing."), "objective body '%s'", a ? a->body : "");
    CHECK(n && !strcmp(n->kind, "document") && !strcmp(n->title, "A note") && !strcmp(n->body, "line one\nline two\n\nafter a gap"), "document body '%s'", n ? n->body : "");
    CHECK(k && !strcmp(k->kind, "voice") && !strcmp(k->speaker, "Ruth") && !strcmp(k->filter, "door") && k->cue_n == 2, "voice meta + 2 cues (%d)", k ? k->cue_n : -1);
    if (k && k->cue_n == 2) {
        const StoryCue *c0 = &sc.cue[k->first_cue], *c1 = &sc.cue[k->first_cue + 1];
        CHECK(!strcmp(c0->speaker, "RUTH") && !strcmp(c0->text, "Hello? It's Ruth."), "cue 0");
        CHECK(!strcmp(c1->speaker, "MR. O'NEIL") && c1->dur == 1.5f && c1->start == c0->dur, "cue 1 timing");
        CHECK(fabsf(c0->dur - story_cue_duration(17)) < 1e-5f && fabsf(k->duration - (c0->dur + c1->dur)) < 1e-5f, "durations");
    }
    CHECK(story_script_find(&sc, "nope") == NULL, "missing id");
    story_script_free(&sc);
    CHECK(!story_script_parse(&sc, "## a\nx\n## a\ny\n", err, sizeof(err)) && strstr(err, "twice"), "duplicate id: %s", err);
    CHECK(!story_script_parse(&sc, "## a [kind\nx\n", err, sizeof(err)) && strstr(err, "unterminated"), "bad meta: %s", err);
}

// ------------------------------------------------------------------ saves

static void test_save_roundtrip(void) {
    StoryDef *d = def_new(OBJ_CHAPTER);
    if (!d) return;
    StoryState *a = malloc(sizeof(StoryState)), *b = malloc(sizeof(StoryState));
    story_init(a, d, 42);
    story_event(a, SEV_INTERACT, "switch");
    run(a, d, NULL, 0.3f);
    story_event(a, SEV_ITEM_READ, "n1");
    story_event(a, SEV_ITEM_READ, "n2");
    run(a, d, NULL, 0.2f);
    story_event(a, SEV_ITEM_PICKUP, "p1");
    story_event(a, SEV_ITEM_PICKUP, "p2");
    run(a, d, NULL, 0.5f); // go_late's sequence is mid-wait
    story_set_flag(a, "custom", 5);
    CHECK(a->seq_n == 1, "a sequence is running (%d)", a->seq_n);
    cJSON *world = cJSON_Parse("{\"doors\":{\"door_10a\":{\"open\":true}},\"containers\":{}}");
    cJSON *players = cJSON_Parse("[{\"role\":\"parent\",\"pos\":[1,30,2],\"inv\":[[\"knife\",1]]}]");
    char *json = story_save_write(a, d, world, players);
    cJSON *w2 = NULL, *p2 = NULL;
    int unknown = -1;
    char err[128];
    CHECK(story_save_read(b, d, json, &w2, &p2, &unknown, err, sizeof(err)), "read: %s", err);
    CHECK(unknown == 0 && story_state_hash(a, d) == story_state_hash(b, d), "same hash after load");
    CHECK(w2 && cJSON_Compare(world, w2, true) && p2 && cJSON_Compare(players, p2, true), "world / players blobs round-trip");
    StorySaveInfo info;
    CHECK(story_save_info(json, &info) && !strcmp(info.chapter, "t") && !strcmp(info.checkpoint, "cp"), "info header");
    // Both continue identically: the loaded sequence resumes its wait.
    for (int i = 0; i < 30; i++) {
        story_tick(a, d, NULL, 0.1f, NULL);
        story_tick(b, d, NULL, 0.1f, NULL);
    }
    CHECK(ost(b, d, "late") == SOS_ACTIVE && story_state_hash(a, d) == story_state_hash(b, d) && story_flag(b, "custom") == 5, "lockstep after load");
    // Wrong chapter / format.
    CHECK(!story_save_read(b, d, "{\"format\":1,\"chapter\":\"other\"}", NULL, NULL, NULL, err, sizeof(err)) && strstr(err, "other"), "wrong chapter: %s", err);
    CHECK(!story_save_read(b, d, "{\"format\":9,\"chapter\":\"t\"}", NULL, NULL, NULL, err, sizeof(err)) && strstr(err, "format"), "future format: %s", err);
    // Edited chapter: unknown ids are skipped and counted.
    cJSON *root = cJSON_Parse(json);
    cJSON_AddItemToObject(cJSON_GetObjectItem(root, "objectives"), "removed_obj", cJSON_Parse("{\"state\":\"done\"}"));
    char *json2 = cJSON_PrintUnformatted(root);
    CHECK(story_save_read(b, d, json2, NULL, NULL, &unknown, err, sizeof(err)) && unknown == 1, "unknown id skipped (%d)", unknown);
    free(json2);
    cJSON_Delete(root);
    cJSON_Delete(world);
    cJSON_Delete(players);
    cJSON_Delete(w2);
    cJSON_Delete(p2);
    free(json);
    free(a);
    free(b);
    free(d);
}

// ------------------------------------------------------------------ shipped chapters + the bot

typedef struct BotRun {
    bool ended;
    float time;
    u32 hash;
    int steps;
    int checkpoints;
} BotRun;

// Runs the bot to the end (or max_seconds). save_every_checkpoint: at each checkpoint note, the state
// is saved, reloaded into a fresh state and the run continues from the copy.
static BotRun bot_run(const StoryDef *d, u32 choice, bool save_every_checkpoint, bool verbose) {
    StoryState *s = malloc(sizeof(StoryState));
    StoryOut *o = malloc(sizeof(StoryOut));
    StoryBot bot;
    story_bot_init(&bot, choice);
    story_init(s, d, 1234);
    StoryView v = story_bot_view(&bot);
    BotRun r = {0};
    const float dt = 1.0f / 30.0f;
    for (int i = 0; i < 30 * 7200 && !s->ended; i++) {   // two hours of story time at most
        int t = story_bot_step(&bot, s, d, dt);
        if (verbose && t != STORY_NONE) printf("  %7.1fs %-24s %s\n", s->now, d->trig[t].id, bot.last_target);
        story_tick(s, d, &v, dt, o);
        for (int k = 0; k < o->count; k++) {
            if (o->n[k].kind != SN_CHECKPOINT) continue;
            r.checkpoints++;
            if (!save_every_checkpoint) continue;
            char *json = story_save_write(s, d, NULL, NULL);
            char err[128];
            StoryState *copy = malloc(sizeof(StoryState));
            bool ok = story_save_read(copy, d, json, NULL, NULL, NULL, err, sizeof(err));
            CHECK(ok && story_state_hash(copy, d) == story_state_hash(s, d), "checkpoint reload hash (%s)", err);
            free(s);
            s = copy;
            free(json);
        }
    }
    r.ended = s->ended;
    r.time = s->now;
    r.hash = story_state_hash(s, d);
    r.steps = bot.steps;
    if (!r.ended) {
        printf("  bot stuck at %.0f s (choice %u); active:", s->now, choice);
        for (int i = 0; i < d->obj_n; i++)
            if (s->obj[i].state == SOS_ACTIVE) printf(" %s", d->obj[i].id);
        printf("\n");
    }
    free(o);
    free(s);
    return r;
}

static void test_chapter_file(const char *path) {
    char *json = read_file(path);
    CHECK(json != NULL, "%s readable (run from the build dir or the repo root)", path);
    if (!json) return;
    StoryDef *d = malloc(sizeof(StoryDef));
    char err[256];
    bool ok = story_def_parse(d, json, err, sizeof(err));
    CHECK(ok, "%s parses: %s", path, err);
    free(json);
    if (!ok) { free(d); return; }
    char out[4096];
    int problems = story_def_validate(d, out, sizeof(out));
    CHECK(problems == 0, "%s lint (%d):\n%s", path, problems, out);
    char *script = read_file(d->script);
    CHECK(script != NULL, "%s: script %s readable", path, d->script);
    if (script) {
        StoryScript sc;
        CHECK(story_script_parse(&sc, script, err, sizeof(err)), "%s parses: %s", d->script, err);
        int missing = story_script_lint(d, &sc, out, sizeof(out));
        CHECK(missing == 0, "%s: %d text ids missing:\n%s", d->script, missing, out);
        story_script_free(&sc);
        free(script);
    }
    // The bot finishes every branch, deterministically, and checkpoints do not change the outcome.
    for (u32 choice = 0; choice < 16; choice++) {
        BotRun r = bot_run(d, choice, false, false);
        CHECK(r.ended, "%s: bot (choice %u) finished (%.0f s, %d steps)", d->id, choice, r.time, r.steps);
        if (!r.ended) {
            bot_run(d, choice, false, true);
            continue;
        }
        CHECK(r.time < 90 * 60, "%s: bot (choice %u) within 90 min (%.0f s)", d->id, choice, r.time);
        // Chapter 1: every morning is a checkpoint (days 2..8), then the street and the end.
        if (!strcmp(d->id, "01_tenth_floor")) CHECK(r.checkpoints == 9, "01: all 7 nights slept, the street, the end (%d checkpoints)", r.checkpoints);
        BotRun again = bot_run(d, choice, false, false);
        CHECK(again.hash == r.hash && again.time == r.time, "deterministic (choice %u)", choice);
        BotRun saved = bot_run(d, choice, true, false);
        CHECK(saved.ended && saved.hash == r.hash && saved.time == r.time && saved.checkpoints == r.checkpoints && r.checkpoints >= 4,
              "save / load at every checkpoint (choice %u: %d checkpoints)", choice, r.checkpoints);
    }
    free(d);
}

// ------------------------------------------------------------------ vista sequences (src/story/vista_seq.c)

typedef struct VistaPts { cJSON *points; } VistaPts;

static bool vista_point(void *user, const char *name, float out[3]) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(((VistaPts *)user)->points, name);
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(p, "pos");
    if (cJSON_GetArraySize(pos) < 3) return false;
    for (int i = 0; i < 3; i++) out[i] = (float)cJSON_GetArrayItem(pos, i)->valuedouble;
    return true;
}

static bool vista_at(VistaPts *pts, const char *name) {
    float p[3];
    return !name[0] || vista_point(pts, name, p);
}

static void test_vista(const char *chapter, const char *level) {
    char *json = read_file("assets/story/vista/sequences.json");
    CHECK(json != NULL, "sequences.json readable");
    if (!json) return;
    static VistaSeqLib lib;
    char err[256];
    CHECK(vista_seq_parse(&lib, json, err, sizeof(err)) && lib.n >= 10, "sequences parse (%d): %s", lib.n, err);
    free(json);
    // Every sequence the chapter starts exists; every point a sequence names is one of the level's.
    StoryDef *d = malloc(sizeof(StoryDef));
    char *cj = read_file(chapter), *lj = read_file(level);
    cJSON *lroot = lj ? cJSON_Parse(lj) : NULL;
    VistaPts pts = { cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(lroot, "story"), "points") };
    CHECK(cj && story_def_parse(d, cj, err, sizeof(err)) && pts.points, "chapter + level for the vista");
    int used = 0;
    for (int i = 0; i < d->act_n; i++) {
        if (d->act[i].type != SA_SEQUENCE) continue;
        used++;
        CHECK(vista_seq_find(&lib, d->act[i].name) != NULL, "sequence '%s' (chapter) defined in sequences.json", d->act[i].name);
    }
    CHECK(used >= 10, "chapter starts %d sequences", used);
    for (int i = 0; i < lib.n; i++) {
        const VistaSeqDef *q = &lib.seq[i];
        CHECK(vista_at(&pts, q->heli_from) && vista_at(&pts, q->heli_to), "%s: heli points in the level", q->name);
        for (int k = 0; k < q->smoke_n; k++) CHECK(vista_at(&pts, q->smoke[k].at), "%s: smoke point '%s' in the level", q->name, q->smoke[k].at);
        for (int k = 0; k < q->fire_n; k++) CHECK(vista_at(&pts, q->fire[k].at), "%s: fire point '%s'", q->name, q->fire[k].at);
        for (int k = 0; k < q->flash_n; k++) CHECK(vista_at(&pts, q->flash[k].at), "%s: flash point '%s'", q->name, q->flash[k].at);
    }
    free(cj);
    free(lj);
    free(d);

    VistaFrame f;
    VistaActive a[4];
    memset(a, 0, sizeof(a));
    vista_eval(&lib, a, 0, vista_point, &pts, &f);
    CHECK(f.power[0] == 1 && f.power[7] == 1 && f.heli_n == 0 && f.smoke_n == 0 && f.birds_scatter_age < 0, "idle city");
    // Halden flickers; the blackout (started later) wins, fading.
    snprintf(a[0].name, VISTA_NAME, "halden_flicker");
    a[0].age = 100;
    snprintf(a[1].name, VISTA_NAME, "district_blackout");
    a[1].age = 1.0f;
    vista_eval(&lib, a, 2, vista_point, &pts, &f);
    CHECK(fabsf(f.power[1] - 1.6f) < 1e-4f && fabsf(f.power[2] - 0.5f) < 1e-4f && f.power[0] == 1, "flicker + blackout half way (%.2f %.2f)", f.power[1], f.power[2]);
    a[1].age = 4.5f;
    vista_eval(&lib, a, 2, vista_point, &pts, &f);
    CHECK(fabsf(f.power[1] - 0.5f) < 1e-4f, "flickering district fades from on (%.2f)", f.power[1]);
    a[1].age = 60;
    vista_eval(&lib, a, 2, vista_point, &pts, &f);
    CHECK(f.power[1] == 0 && f.power[2] == 0 && fabsf(f.power[0] - 0.15f) < 1e-4f && fabsf(f.power[6] - 0.1f) < 1e-4f, "blackout done");
    // The helicopter pass: from a to b, then gone.
    float pa[3], pb[3];
    vista_point(&pts, "heli1_a", pa);
    vista_point(&pts, "heli1_b", pb);
    snprintf(a[0].name, VISTA_NAME, "heli_north_pass");
    a[0].age = 22.5f;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.heli_n == 1 && fabsf(f.heli[0].pos[0] - (pa[0] + pb[0]) * 0.5f) < 0.5f && fabsf(f.heli[0].pos[2] - (pa[2] + pb[2]) * 0.5f) < 0.5f, "heli half way");
    float fwd[2] = { -sinf(f.heli[0].yaw), -cosf(f.heli[0].yaw) }, dir[2] = { pb[0] - pa[0], pb[2] - pa[2] };
    CHECK(fwd[0] * dir[0] + fwd[1] * dir[1] > 0.99f * sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), "heli faces its way");
    a[0].age = 45.1f;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.heli_n == 0, "heli gone after the pass");
    // The crash: spirals onto the hospital roof at 5.2 s; then the flash, a fire, smoke.
    float ph[3];
    vista_point(&pts, "vista_hospital", ph);
    snprintf(a[0].name, VISTA_NAME, "heli_crash");
    a[0].age = 0;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.heli_n == 1 && f.heli[0].crashing && fabsf(hypotf(f.heli[0].pos[0] - ph[0], f.heli[0].pos[2] - ph[2]) - 260.0f) < 1.0f && f.flash_n == 0, "crash starts 260 m out");
    a[0].age = 5.19f;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.heli_n == 1 && hypotf(f.heli[0].pos[0] - ph[0], f.heli[0].pos[2] - ph[2]) < 2.0f && f.heli[0].pos[1] - ph[1] < 1.0f, "crash ends on the roof");
    a[0].age = 6.0f;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.heli_n == 0 && f.flash_n == 1 && f.fire_n == 1 && f.smoke_n == 1 && fabsf(f.smoke[0].age - 0.8f) < 1e-4f, "after the crash: flash, fire, smoke");
    a[0].age = 9.0f;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.flash_n == 0 && f.fire_n == 1, "the flash is over, the fire burns on");
    // Smoke: deterministic particles that rise; none outside the live window.
    snprintf(a[0].name, VISTA_NAME, "smoke_aldric");
    a[0].age = 200;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.smoke_n == 1, "aldric column");
    int alive = 0, n = vista_smoke_count(&f.smoke[0]);
    float top = 0, p[3], p2[3], sz, sz2, sh, al, al2;
    for (int k = 0; k < n + 10; k++) {
        if (!vista_smoke_particle(&f.smoke[0], k, p, &sz, &sh, &al)) continue;
        alive++;
        top = fmaxf(top, p[1]);
        bool again = vista_smoke_particle(&f.smoke[0], k, p2, &sz2, &sh, &al2);
        CHECK(again && p2[1] == p[1] && sz2 == sz && al2 == al && al >= 0 && al <= 1 && sz > 0, "smoke particle %d deterministic", k);
        if (g_fail > 20) break;
    }
    CHECK(alive > 60 && alive < 120 && top > ph[1] + 150, "column: %d particles, top %.0f m", alive, top);
    // Traffic: build then stall.
    snprintf(a[0].name, VISTA_NAME, "traffic_build");
    a[0].age = 1000;
    snprintf(a[1].name, VISTA_NAME, "traffic_stall");
    a[1].age = 45;
    vista_eval(&lib, a, 2, vista_point, &pts, &f);
    CHECK(fabsf(f.traffic_density - 0.8f) < 1e-4f && fabsf(f.traffic_stall - 0.425f) < 1e-4f && f.stall_age == 45, "traffic (%.2f %.3f)", f.traffic_density, f.traffic_stall);
    snprintf(a[0].name, VISTA_NAME, "birds_scatter");
    a[0].age = 3;
    vista_eval(&lib, a, 1, vista_point, &pts, &f);
    CHECK(f.birds_scatter_age == 3, "birds scatter");
    cJSON_Delete(lroot);
}

int main(int argc, char **argv) {
    test_clock_parse();
    test_parse_errors();
    test_lint();
    test_objectives();
    test_triggers();
    test_clock();
    test_script();
    test_save_roundtrip();
    test_chapter_file("assets/story/chapters/01_tenth_floor.json");
    test_vista("assets/story/chapters/01_tenth_floor.json", "data/levels/story_01.json");
    if (argc > 1) { // test_story <chapter.json> --trace <choice>: print the bot's path
        StoryDef *d = malloc(sizeof(StoryDef));
        char *json = read_file(argv[1]);
        char err[256];
        if (json && story_def_parse(d, json, err, sizeof(err))) bot_run(d, argc > 3 ? (u32)atoi(argv[3]) : 0, false, true);
        free(json);
        free(d);
    }
    printf("test_story: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
