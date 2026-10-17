// Unit tests for the pure revive core (src/gameplay/revive/revive_core.c, death_seq.c, docs/REVIVE.md
// "Tests"): down -> revive / bleed-out -> dead -> extracted -> inserted -> reconstructing -> alive with
// every interrupt, exact 3 / 5 / 6 / 10 s timers at 60 Hz, the 0.6 m corpse gate, one sample per
// corpse, sample rules, fabricator queue order + power loss, late-join progress, predicted action
// reconcile, and the death sequence (stage 1 target time, one-frame black cut, pop-free ring fade,
// neutral after revive, impact detector paths).
#include "gameplay/revive/revive_core.h"
#include "gameplay/revive/death_seq.h"
#include "audio/death_synth.h"
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

// ------------------------------------------------------------------ rules

static void test_rules(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    CHECK(r.revive_time == 6.0f && r.extract_time == 3.0f && r.insert_time == 5.0f && r.recon_time == 10.0f, "default timings");
    CHECK(r.corpse_range == 0.6f && r.corpse_lifetime == 600.0f, "default corpse");
    const char *j = "{\"downed\":{\"bleed_time\":30,\"keep_sidearm\":false},\"revive\":{\"time\":4},"
                    "\"corpse\":{\"samples\":2},\"sample\":{\"on_carrier_death\":\"lost\"},"
                    "\"fabricator\":{\"queue_max\":9,\"respawn_items\":[[\"bandage\",2],\"flashlight\"]},\"respawn_mode\":\"fabricator\"}";
    char err[64];
    CHECK(revive_rules_parse(&r, j, err, sizeof(err)), "parse");
    CHECK(r.bleed_time == 30.0f && !r.keep_sidearm && r.revive_time == 4.0f, "overrides");
    CHECK(r.samples_per_corpse == 2 && r.sample_on_death == SAMPLE_LOST, "corpse / sample rules");
    CHECK(r.queue_max == FAB_QUEUE_MAX, "queue clamped to the wire size (%d)", r.queue_max);
    CHECK(r.respawn_item_n == 2 && !strcmp(r.respawn_items[0], "bandage") && r.respawn_item_counts[0] == 2 && r.respawn_item_counts[1] == 1, "respawn items");
    CHECK(r.respawn_mode == RESPAWN_FABRICATOR, "respawn mode");
    CHECK(r.extract_time == 3.0f, "untouched keys keep defaults");
    CHECK(!revive_rules_parse(&r, "{nope", err, sizeof(err)) && r.revive_time == 6.0f, "bad json -> defaults");
    // The shipped file parses.
    char *text = read_file("assets/gameplay/revive_rules.json");
    if (!text) text = read_file("../assets/gameplay/revive_rules.json");
    CHECK(text && revive_rules_parse(&r, text, err, sizeof(err)), "assets/gameplay/revive_rules.json parses");
    free(text);
}

// ------------------------------------------------------------------ downed

static void test_downed(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    DownState d;
    down_clear(&d);
    CHECK(down_should_knock(&r, &d, false, 0), "0 hp knocks");
    CHECK(!down_should_knock(&r, &d, true, 0), "kill-type damage kills");
    r.downs_enabled = false;
    CHECK(!down_should_knock(&r, &d, false, 0), "downs disabled by rule");
    r.downs_enabled = true;
    r.overkill = 50;
    CHECK(!down_should_knock(&r, &d, false, 80), "overkill");
    CHECK(down_should_knock(&r, &d, false, 20), "under the overkill");
    r.overkill = 0;

    down_enter(&d, &r);
    CHECK(d.downed && d.downs == 1 && d.bleed_total == 3600, "first down: 60 s = 3600 ticks (%u)", d.bleed_total);
    CHECK(!down_should_knock(&r, &d, false, 0), "a downed player dies on lethal damage");
    // Bleed-out is exact.
    int ticks = 0;
    while (!down_step(&d, &r) && ticks < 100000) ticks++;
    CHECK(ticks + 1 == 3600, "bleeds out on tick 3600 (%d)", ticks + 1);
    // Shorter each down, with a floor.
    CHECK(down_bleed_total(&r, 1) == 2160, "second down 36 s (%u)", down_bleed_total(&r, 1));
    CHECK(down_bleed_total(&r, 5) == 900, "floor 15 s (%u)", down_bleed_total(&r, 5));
    // Being revived pauses the bleed-out.
    down_clear(&d);
    down_enter(&d, &r);
    d.reviver = 2;
    for (int i = 0; i < 100; i++) down_step(&d, &r);
    CHECK(d.bleed_ticks == d.bleed_total, "paused while revived");
    d.reviver = REVIVE_NOBODY;
    down_step(&d, &r);
    CHECK(d.bleed_ticks == d.bleed_total - 1, "resumes");
    down_revived(&d, &r);
    CHECK(!d.downed && d.downs == 1, "revived keeps the down count");
    r.max_downs = 1;
    CHECK(!down_should_knock(&r, &d, false, 0), "max downs reached -> dead");
    r.max_downs = 0;
    r.revive_resets_downs = true;
    down_enter(&d, &r);
    down_revived(&d, &r);
    CHECK(d.downs == 0, "resets downs (rule)");
}

// ------------------------------------------------------------------ actions

static ActInput hold(u32 target, ActKind k, u16 dur) {
    ActInput in;
    memset(&in, 0, sizeof(in));
    in.use_down = true;
    in.target = target;
    in.afford = k;
    in.target_ok = true;
    in.move_tolerance = 0.35f;
    in.dur = dur;
    return in;
}

static int run_until_done(ActionState *a, ActInput in, int max) {
    in.use_pressed = true;
    if (act_step(a, &in) != ACTR_STARTED) return -1;
    in.use_pressed = false;
    for (int t = 1; t <= max; t++) {
        ActResult res = act_step(a, &in);
        if (res == ACTR_DONE) return t;
        if (res == ACTR_CANCELLED) return -2;
    }
    return -3;
}

static void test_actions(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    struct { ActKind k; float s; int ticks; } cases[] = {
        { RACT_EXTRACT, r.extract_time, 180 }, { RACT_INSERT, r.insert_time, 300 }, { RACT_REVIVE, r.revive_time, 360 },
    };
    for (int c = 0; c < 3; c++) {
        ActionState a;
        memset(&a, 0, sizeof(a));
        int t = run_until_done(&a, hold(77, cases[c].k, revive_ticks(cases[c].s)), 1000);
        CHECK(t == cases[c].ticks, "%s: done on tick %d (want %d)", act_kind_name(cases[c].k), t, cases[c].ticks);
        CHECK(a.phase == ACTP_DONE, "phase done");
        // Holding on does not restart it.
        ActInput in = hold(77, cases[c].k, revive_ticks(cases[c].s));
        in.use_pressed = true;
        CHECK(act_step(&a, &in) == ACTR_NONE && a.phase == ACTP_DONE, "no restart while held");
        in.use_down = in.use_pressed = false;
        act_step(&a, &in);
        CHECK(a.phase == ACTP_IDLE, "release -> idle");
    }
    // Interrupts.
    const char *names[] = { "release", "move", "damage", "range" };
    for (int k = 0; k < 4; k++) {
        ActionState a;
        memset(&a, 0, sizeof(a));
        ActInput in = hold(5, RACT_EXTRACT, 180);
        in.use_pressed = true;
        act_step(&a, &in);
        in.use_pressed = false;
        for (int t = 0; t < 60; t++) act_step(&a, &in);
        if (k == 0) in.use_down = false;
        if (k == 1) in.pos = v3(0.5f, 0, 0);
        if (k == 2) in.damaged = true;
        if (k == 3) in.target_ok = false;
        ActCancel want[] = { ACTC_RELEASE, ACTC_MOVE, ACTC_DAMAGE, ACTC_RANGE };
        CHECK(act_step(&a, &in) == ACTR_CANCELLED && a.cancel == want[k], "interrupt: %s (%s)", names[k], act_cancel_name((ActCancel)a.cancel));
    }
    // Small moves inside the tolerance are fine; vertical (crouch) never counts.
    {
        ActionState a;
        memset(&a, 0, sizeof(a));
        ActInput in = hold(5, RACT_EXTRACT, 180);
        in.pos = v3(0, 0, 0);
        in.use_pressed = true;
        act_step(&a, &in);
        in.use_pressed = false;
        in.pos = v3(0.2f, -0.6f, 0.1f);
        CHECK(act_step(&a, &in) == ACTR_PROGRESS, "within tolerance");
    }
    // Nothing to afford / no press.
    {
        ActionState a;
        memset(&a, 0, sizeof(a));
        ActInput in = hold(5, RACT_NONE, 180);
        in.use_pressed = true;
        CHECK(act_step(&a, &in) == ACTR_NONE, "nothing affordable");
        in = hold(5, RACT_EXTRACT, 180);
        CHECK(act_step(&a, &in) == ACTR_NONE, "needs a fresh press");
    }
    // Host cancel (sample gone, target revived by someone else).
    {
        ActionState a;
        memset(&a, 0, sizeof(a));
        ActInput in = hold(5, RACT_REVIVE, 360);
        in.use_pressed = true;
        act_step(&a, &in);
        act_cancel(&a, ACTC_HOST);
        CHECK(a.phase == ACTP_CANCELLED && a.cancel == ACTC_HOST, "host cancel");
    }
}

// Predicted ring reconcile: the client runs ahead, the host state for an older tick arrives and the
// client replays its newer inputs; it must land on the same progress as uninterrupted prediction,
// and a host interrupt must win.
static void test_reconcile(void) {
    ActionState host, client;
    memset(&host, 0, sizeof(host));
    memset(&client, 0, sizeof(client));
    ActInput inputs[200];
    for (int t = 0; t < 200; t++) {
        inputs[t] = hold(9, RACT_EXTRACT, 180);
        inputs[t].use_pressed = t == 0;
    }
    for (int t = 0; t < 120; t++) act_step(&client, &inputs[t]);   // client predicted 120 ticks
    for (int t = 0; t < 100; t++) act_step(&host, &inputs[t]);     // host acked 100 (latency)
    ActionState replay = host;
    for (int t = 100; t < 120; t++) act_step(&replay, &inputs[t]);
    CHECK(!memcmp(&replay, &client, sizeof(replay)), "replay matches prediction (%u vs %u)", replay.ticks, client.ticks);
    CHECK(client.ticks == 119, "ring started on the first tick (predicted, %u)", client.ticks);
    // Host interrupt on tick 100 (damage): the replay keeps it cancelled.
    ActionState h2 = host;
    act_cancel(&h2, ACTC_HOST);
    ActionState r2 = h2;
    for (int t = 100; t < 120; t++) act_step(&r2, &inputs[t]);
    CHECK(r2.phase == ACTP_CANCELLED, "host interrupt survives the replay");
}

// ------------------------------------------------------------------ corpse

static void test_corpse(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    vec3 pelvis = v3(0, 0.15f, 0), head = v3(0, 0.15f, 0.9f);
    CHECK(corpse_in_range(&r, v3(0.55f, 0, 0.4f), pelvis, head), "0.55 m beside the body");
    CHECK(!corpse_in_range(&r, v3(0.7f, 0, 0.4f), pelvis, head), "0.7 m: get closer");
    CHECK(corpse_in_range(&r, v3(0, 0, 1.45f), pelvis, head), "0.55 m past the head");
    CHECK(!corpse_in_range(&r, v3(0, 0, -0.65f), pelvis, head), "0.65 m behind the pelvis");
    CHECK(!corpse_in_range(&r, v3(0.2f, 2.0f, 0.4f), pelvis, head), "standing on a floor above");
    CHECK(fabsf(corpse_distance(v3(0.3f, 0, 0.5f), pelvis, head) - 0.3f) < 1e-5f, "distance to the segment");

    CorpseState c;
    corpse_init(&c, &r, 3, 1000);
    CHECK(corpse_dna_available(&c) && c.owner_slot == 3, "fresh corpse");
    CHECK(corpse_extract(&c, 1, 1100), "first sample");
    CHECK(!corpse_dna_available(&c) && !corpse_extract(&c, 2, 1101), "one sample per corpse");
    CHECK(c.extractor == 1 && c.extract_tick == 1100, "extractor recorded");
    CHECK(!corpse_should_despawn(&c, &r, 1100 + 20 * 60 - 1), "stays for the despawn delay");
    CHECK(corpse_should_despawn(&c, &r, 1100 + 20 * 60), "despawns 20 s after the extraction");
    corpse_init(&c, &r, 3, 1000);
    CHECK(!corpse_should_despawn(&c, &r, 1000 + 600 * 60 - 1) && corpse_should_despawn(&c, &r, 1000 + 600 * 60), "lifetime 10 min");
    r.samples_per_corpse = 2;
    corpse_init(&c, &r, 3, 0);
    CHECK(corpse_extract(&c, 1, 5) && corpse_dna_available(&c) && corpse_extract(&c, 2, 6) && !corpse_dna_available(&c), "rule: two samples");
}

// ------------------------------------------------------------------ DNA

static void test_dna(void) {
    DnaRegistry g;
    dna_reset(&g);
    int a = dna_alloc(&g, 2, "Alex", 1, 0xABCD, 55);
    int b = dna_alloc(&g, 2, "Alex", 3, 0xABCD, 56);
    CHECK(a == 1 && b == 2, "ids %d %d", a, b);
    CHECK(dna_get(&g, a)->uid != dna_get(&g, b)->uid, "unique uids");
    CHECK(!strcmp(dna_get(&g, a)->name, "Alex") && dna_get(&g, a)->carrier == 1, "name + carrier");
    CHECK(dna_find_player(&g, 2) == b, "newest sample of a player");
    dna_free(&g, a);
    CHECK(!dna_get(&g, a) && dna_get(&g, b), "free");
    CHECK(dna_alloc(&g, 4, "Kim", 0, 0, 0) == a, "slot reused");
    CHECK(dna_get(&g, a)->uid == 3, "uid never reused (%u)", dna_get(&g, a)->uid);
    for (int i = 0; i < DNA_MAX; i++) dna_alloc(&g, 1, "x", 0, 0, 0);
    CHECK(dna_alloc(&g, 1, "x", 0, 0, 0) == 0, "full registry");
    CHECK(!dna_get(&g, 0) && !dna_get(&g, DNA_MAX + 1), "bounds");
}

// ------------------------------------------------------------------ fabricator

static void test_fabricator(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    FabMachine f;
    fab_init(&f, true);
    CHECK(fab_enqueue(&f, &r, 4) && fab_enqueue(&f, &r, 7) && fab_enqueue(&f, &r, 2), "queue three");
    CHECK(!fab_enqueue(&f, &r, 7), "no duplicates");
    u32 t = 1000;
    CHECK(fab_step(&f, &r, t) == FABE_RECON_START && f.current == 4 && f.qn == 2, "oldest first");
    CHECK(f.queue[0] == 7 && f.queue[1] == 2, "queue order kept");
    int ticks = 0;
    FabEvent e = FABE_NONE;
    while ((e = fab_step(&f, &r, ++t)) == FABE_NONE) ticks++;
    CHECK(e == FABE_RESPAWN && ticks + 1 == 600, "reconstruction is exactly 10 s (%d ticks)", ticks + 1);
    CHECK(fabsf(fab_progress(&f, &r, t) - 1.0f) < 1e-6f, "progress 1 while opening");
    while ((e = fab_step(&f, &r, ++t)) == FABE_NONE) {}
    CHECK(e == FABE_OPENED && f.current == 0, "opened");
    while ((e = fab_step(&f, &r, ++t)) == FABE_NONE) {}
    CHECK(e == FABE_READY, "cooldown over");
    CHECK(fab_step(&f, &r, ++t) == FABE_RECON_START && f.current == 7, "next in queue");
    // Late join: progress from the replicated start tick alone.
    FabMachine late;
    memset(&late, 0, sizeof(late));
    late.state = f.state;
    late.state_tick = f.state_tick;
    CHECK(fabsf(fab_progress(&late, &r, t + 300) - 0.5f) < 1e-6f, "late joiner sees 50 %% at 5 s");
    CHECK(fabsf(fab_remaining(&late, &r, t + 300) - 5.0f) < 1e-4f, "5 s remaining");
    // Power loss mid-cycle ejects the current + queued samples.
    u8 ej[8];
    int n = fab_set_power(&f, &r, false, t + 10, ej, 8);
    CHECK(n == 2 && ej[0] == 7 && ej[1] == 2, "ejected %d", n);
    CHECK(f.state == FAB_UNPOWERED && f.qn == 0 && f.current == 0, "unpowered + empty");
    CHECK(fab_step(&f, &r, t + 20) == FABE_NONE, "no progress without power");
    r.require_power = true;
    CHECK(!fab_enqueue(&f, &r, 3), "no insert without power (rule)");
    fab_set_power(&f, &r, true, t + 30, ej, 8);
    CHECK(f.state == FAB_IDLE && fab_enqueue(&f, &r, 3), "power back");
    // Queue cap.
    fab_init(&f, true);
    r.queue_max = 2;
    CHECK(fab_enqueue(&f, &r, 1) && fab_enqueue(&f, &r, 2) && !fab_enqueue(&f, &r, 3), "queue cap");
    CHECK(fab_remove(&f, 1) && f.qn == 1 && f.queue[0] == 2, "remove keeps order");
}

// The whole loop through the pure machines: down -> bleed out -> dead -> extract -> insert -> respawn.
static void test_full_loop(void) {
    ReviveRules r;
    revive_rules_defaults(&r);
    DownState d;
    down_clear(&d);
    CHECK(down_should_knock(&r, &d, false, 0), "knocked");
    down_enter(&d, &r);
    int t = 0;
    while (!down_step(&d, &r)) t++;
    CorpseState c;
    corpse_init(&c, &r, 1, (u32)t);
    ActionState a;
    memset(&a, 0, sizeof(a));
    CHECK(run_until_done(&a, hold(99, RACT_EXTRACT, revive_ticks(r.extract_time)), 400) == 180, "extracted");
    CHECK(corpse_extract(&c, 0, (u32)t + 180), "corpse sample");
    DnaRegistry g;
    dna_reset(&g);
    int id = dna_alloc(&g, 1, "Bo", 0, 0, 99);
    memset(&a, 0, sizeof(a));
    CHECK(run_until_done(&a, hold(50, RACT_INSERT, revive_ticks(r.insert_time)), 400) == 300, "inserted");
    FabMachine f;
    fab_init(&f, true);
    CHECK(fab_enqueue(&f, &r, id), "queued");
    u32 tick = 5000;
    CHECK(fab_step(&f, &r, tick) == FABE_RECON_START, "reconstructing");
    FabEvent e;
    while ((e = fab_step(&f, &r, ++tick)) == FABE_NONE) {}
    CHECK(e == FABE_RESPAWN && tick == 5600, "alive again after 10 s");
    down_clear(&d);
    CHECK(!d.downed && d.downs == 0, "respawn clears downs");
}

// ------------------------------------------------------------------ death sequence

static void test_death_seq(void) {
    DeathSeqParams p;
    death_params_defaults(&p);
    char err[64];
    char *text = read_file("assets/vfx/death_sequence.json");
    if (!text) text = read_file("../assets/vfx/death_sequence.json");
    CHECK(text && death_params_parse_visual(&p, text, err, sizeof(err)), "death_sequence.json parses");
    free(text);
    text = read_file("assets/audio/death_ring.json");
    if (!text) text = read_file("../assets/audio/death_ring.json");
    CHECK(text && death_params_parse_ring(&p, text, err, sizeof(err)), "death_ring.json parses");
    free(text);

    const float dt = 1.0f / 60.0f;
    DeathSeq s;
    memset(&s, 0, sizeof(s));
    DeathFx n = deathseq_eval(&s, &p);
    CHECK(n.desat == 0 && n.black == 0 && n.ring_gain == 0 && n.world_gain == 1 && n.lpf_hz == p.lpf_from, "neutral before death");

    deathseq_start(&s);
    DeathFx f0 = deathseq_eval(&s, &p);
    CHECK(f0.black == 0 && f0.ring_gain == 0, "frame 0");
    // Stage 1 reaches its desaturated target within s1_time.
    int frames = 0;
    while (s.t < p.s1_time - 1e-4f) { deathseq_step(&s, &p, dt, false); frames++; }
    DeathFx f1 = deathseq_eval(&s, &p);
    CHECK(f1.desat >= p.desat * 0.98f, "desaturated in %.2f s (%.3f of %.3f)", s.t, f1.desat, p.desat);
    CHECK(f1.black == 0, "the view stays visible during the fall");
    CHECK(f1.ring_gain >= p.ring_level * 0.99f && f1.lpf_hz < 1500.0f, "ring up, low-pass sweeping (%.0f Hz)", f1.lpf_hz);
    {
        DeathSeq z = s;
        while (z.t < p.lpf_sweep + 1e-4f) deathseq_step(&z, &p, dt, false);
        CHECK(fabsf(deathseq_eval(&z, &p).lpf_hz - p.lpf_to) < 1.0f, "cutoff at %.0f Hz after the sweep", p.lpf_to);
    }
    CHECK(f1.world_gain < 1.0f && f1.world_gain > 0.0f, "world ducked, still audible");
    // Ring: no step larger than the attack slope allows (no pop at the start).
    {
        DeathSeq z;
        memset(&z, 0, sizeof(z));
        deathseq_start(&z);
        float prev = 0, maxd = 0;
        for (int i = 0; i < 60; i++) {
            deathseq_step(&z, &p, dt, false);
            float g = deathseq_eval(&z, &p).ring_gain;
            maxd = fmaxf(maxd, fabsf(g - prev));
            prev = g;
        }
        CHECK(maxd < p.ring_level * 0.15f, "ring attack is smooth (max step %.4f)", maxd);
    }
    // Black cut: exactly one frame from visible to black, flagged as a camera cut once.
    for (int i = 0; i < 30; i++) deathseq_step(&s, &p, dt, false);
    DeathFx before = deathseq_eval(&s, &p);
    deathseq_impact(&s, IMPACT_CONTACT);
    DeathFx cut = deathseq_eval(&s, &p);
    CHECK(before.black == 0 && cut.black == 1, "cut in one frame (%.2f -> %.2f)", before.black, cut.black);
    CHECK(cut.camera_cut, "TAA history reset on the cut");
    deathseq_frame_done(&s);
    deathseq_step(&s, &p, dt, false);
    DeathFx after = deathseq_eval(&s, &p);
    CHECK(!after.camera_cut && after.black == 1, "the cut flag lasts one frame");
    CHECK(fabsf(cut.ring_gain - before.ring_gain) < 1e-4f, "ring continuous across the cut");
    CHECK(after.heart_gain == 0, "heartbeat stops");
    // Ring fades to silence over ring_fade with no pops.
    float prev = cut.ring_gain, maxd = 0;
    float t_black = 0;
    while (s.stage == DS_BLACK) {
        deathseq_step(&s, &p, dt, false);
        DeathFx f = deathseq_eval(&s, &p);
        deathseq_frame_done(&s);
        if (s.stage == DS_BLACK) {
            maxd = fmaxf(maxd, fabsf(f.ring_gain - prev));
            prev = f.ring_gain;
            if (t_black < p.ring_fade - 1e-3f) CHECK(f.black == 1, "black holds");
        }
        t_black += dt;
    }
    CHECK(prev < 1e-4f, "ring silent before the fade-in (%.5f)", prev);
    CHECK(maxd < p.ring_level * 0.05f, "ring fade is pop-free (max step %.4f)", maxd);
    CHECK(fabsf(t_black - (p.ring_fade + p.hold_time)) < 2.5f * dt, "black lasts fade + hold (%.2f s)", t_black);
    CHECK(s.stage == DS_FADE_IN, "fade into spectating");
    while (s.stage == DS_FADE_IN) deathseq_step(&s, &p, dt, false);
    DeathFx done = deathseq_eval(&s, &p);
    CHECK(done.black == 0 && done.desat == 0 && done.world_gain == 1 && done.ring_gain == 0, "neutral after the fade-in");

    // Timeout fallback when the body never settles.
    memset(&s, 0, sizeof(s));
    deathseq_start(&s);
    for (int i = 0; i < 200 && s.stage == DS_FALL; i++) deathseq_step(&s, &p, dt, false);
    CHECK(s.stage == DS_BLACK && s.impact_reason == IMPACT_TIMEOUT && fabsf(s.impact_t - p.impact_timeout) < 2 * dt, "timeout cut at %.2f s", s.impact_t);

    // Impact detector: contact after the minimum time; velocity drop fallback.
    memset(&s, 0, sizeof(s));
    deathseq_start(&s);
    deathseq_step(&s, &p, dt, false);
    CHECK(death_impact_detect(&s, &p, 2.0f, 2.0f, true) == IMPACT_NONE, "contact right after death ignored");
    for (int i = 0; i < 20; i++) { deathseq_step(&s, &p, dt, false); death_impact_detect(&s, &p, 3.0f + 0.1f * (float)i, 2.5f, false); }
    CHECK(s.stage == DS_FALL, "still falling");
    CHECK(death_impact_detect(&s, &p, 2.0f, 1.0f, true) == IMPACT_CONTACT && s.stage == DS_BLACK, "head contact cuts");
    memset(&s, 0, sizeof(s));
    deathseq_start(&s);
    for (int i = 0; i < 20; i++) { deathseq_step(&s, &p, dt, false); death_impact_detect(&s, &p, 4.0f, 3.0f, false); }
    CHECK(death_impact_detect(&s, &p, 0.5f, 0.4f, false) == IMPACT_VELOCITY, "velocity drop fallback");
    memset(&s, 0, sizeof(s));
    deathseq_start(&s);
    for (int i = 0; i < 20; i++) deathseq_step(&s, &p, dt, false);
    CHECK(death_impact_detect(&s, &p, 0.0f, 0.0f, true) == IMPACT_NONE, "a still body touching the ground waits for the timeout");

    // Downed light variant: partial, no black, recovers on revive (reverse curve) to neutral.
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 120; i++) deathseq_step(&s, &p, dt, true);
    DeathFx dn = deathseq_eval(&s, &p);
    CHECK(dn.desat > 0.3f && dn.desat < 0.9f && dn.black == 0 && dn.lpf_hz < 5000 && dn.ring_gain > 0, "downed variant (desat %.2f, lpf %.0f)", dn.desat, dn.lpf_hz);
    for (int i = 0; i < 120; i++) deathseq_step(&s, &p, dt, false);
    DeathFx rv = deathseq_eval(&s, &p);
    CHECK(rv.desat == 0 && rv.ring_gain == 0 && rv.lpf_hz == p.lpf_from && rv.world_gain == 1, "revive restores neutral");
    // revive / respawn mid-sequence: reset is immediate.
    deathseq_start(&s);
    for (int i = 0; i < 10; i++) deathseq_step(&s, &p, dt, false);
    deathseq_reset(&s);
    DeathFx rs = deathseq_eval(&s, &p);
    CHECK(rs.desat == 0 && rs.black == 0 && rs.ring_gain == 0 && rs.world_gain == 1, "`revive` resets post + audio");
    // Skip.
    deathseq_start(&s);
    deathseq_skip(&s);
    CHECK(s.stage == DS_FADE_IN && s.skipped, "skippable");
    // Preview scrub covers the stages.
    CHECK(deathseq_preview(&p, 1, 1.0f).desat >= p.desat * 0.98f, "preview stage 1");
    CHECK(deathseq_preview(&p, 2, 0.1f).black == 1.0f && deathseq_preview(&p, 2, 1.0f).black < 0.01f, "preview stage 2");
    CHECK(deathseq_preview(&p, 3, 1.0f).desat > 0.3f, "preview downed");
    // No NaNs anywhere on the timeline.
    memset(&s, 0, sizeof(s));
    deathseq_start(&s);
    bool finite = true;
    for (int i = 0; i < 600; i++) {
        deathseq_step(&s, &p, dt, i < 100);
        DeathFx f = deathseq_eval(&s, &p);
        float v[] = { f.desat, f.contrast, f.vignette, f.blur, f.exposure, f.black, f.lpf_hz, f.world_gain, f.ring_gain, f.heart_gain, f.heart_bpm };
        for (int k = 0; k < (int)(sizeof(v) / sizeof(v[0])); k++) finite = finite && isfinite(v[k]);
    }
    CHECK(finite, "finite curves");
}

// ------------------------------------------------------------------ death synth (ring + heartbeat)

static void test_synth(void) {
    const float sr = 48000.0f;
    static float buf[48000 * 2];
    DeathSynth s;
    death_synth_init(&s);
    memset(buf, 0, sizeof(buf));
    death_synth_render(&s, buf, 48000, 2, sr);
    float mx = 0;
    for (int i = 0; i < 96000; i++) mx = fmaxf(mx, fabsf(buf[i]));
    CHECK(mx == 0.0f, "silent until targeted");
    // Ring at 3600 Hz, no warble: the zero crossings give the pitch.
    DeathSynthParams p = { 0.3f, 3600.0f, 0.0f, 0.0f, 0.0f, 70.0f };
    death_synth_set(&s, &p);
    memset(buf, 0, sizeof(buf));
    death_synth_render(&s, buf, 48000, 2, sr);
    int zc = 0;
    for (int i = 24000; i < 48000 - 1; i++) if ((buf[i * 2] < 0) != (buf[i * 2 + 2] < 0)) zc++;
    float hz = (float)zc / 2.0f / 0.5f;
    CHECK(fabsf(hz - 3600.0f) < 40.0f, "ring pitch %.0f Hz", hz);
    // A gain step (the ring starting at death, stopping at the cut) never jumps: max sample delta
    // stays near the tone's own slope (2*pi*f/sr * amplitude).
    float slope = (float)(6.2831853 * 3600.0 / sr) * 0.3f * 0.5f * 1.16f;   // sin + 0.08 sin(2x): slope factor 1.16
    float maxd = 0;
    for (int i = 1; i < 48000; i++) maxd = fmaxf(maxd, fabsf(buf[i * 2] - buf[i * 2 - 2]));
    CHECK(maxd <= slope * 1.05f, "no pop on the attack (max delta %.4f, tone slope %.4f)", maxd, slope);
    p.ring_gain = 0.0f;
    death_synth_set(&s, &p);
    memset(buf, 0, sizeof(buf));
    death_synth_render(&s, buf, 24000, 2, sr);
    maxd = 0;
    for (int i = 1; i < 24000; i++) maxd = fmaxf(maxd, fabsf(buf[i * 2] - buf[i * 2 - 2]));
    CHECK(maxd <= slope * 1.05f, "no pop on the stop (max delta %.4f)", maxd);
    CHECK(fabsf(buf[23999 * 2]) < 1e-4f && death_synth_silent(&s), "silent after the release");
    // Heartbeat: periodic low thumps at the target rate.
    DeathSynthParams h = { 0.0f, 3600.0f, 0.0f, 0.0f, 0.6f, 60.0f };
    death_synth_init(&s);
    death_synth_set(&s, &h);
    memset(buf, 0, sizeof(buf));
    death_synth_render(&s, buf, 48000, 1, sr);
    float e_beat = 0, e_gap = 0;
    for (int i = 0; i < 4800; i++) e_beat += buf[i] * buf[i];             // 0..100 ms: the lub
    for (int i = 33600; i < 38400; i++) e_gap += buf[i] * buf[i];        // 700..800 ms: between beats
    CHECK(e_beat > 50.0f * fmaxf(e_gap, 1e-9f), "heartbeat pulses (%.3f vs %.6f)", e_beat, e_gap);
    bool finite = true;
    for (int i = 0; i < 48000; i++) finite = finite && isfinite(buf[i]);
    CHECK(finite, "finite output");
}

int main(void) {
    test_rules();
    test_downed();
    test_actions();
    test_reconcile();
    test_corpse();
    test_dna();
    test_fabricator();
    test_full_loop();
    test_death_seq();
    test_synth();
    printf("test_revive: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
