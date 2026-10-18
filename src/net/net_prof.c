// Netcode statistics for the profiler (net_prof.h).
#include "net_prof.h"
#include "replication.h"
#include "protocol.h"
#include "core/profiler.h"
#include <stdio.h>
#include <string.h>

#define NP_CLIENTS 8
#define NP_FIELDS 15
#define NP_SUB 6

static const char *k_section_names[NS_COUNT] = { "header", "local", "spectate", "time", "weather", "vehicles", "entities" };
static const char *k_field_names[NP_FIELDS] = { "type", "flags", "pos", "rot", "anim", "item", "hp", "vis", "name", "light", "extra", "animst", "attach", "dpos", "dpos8" };
// Fixed payload sizes (write_fields in replication.c); 0 = variable (name, animst, attach).
static const u32 k_field_size[NP_FIELDS] = { 2, 4, 12, 4, 4, 4, 12, 20, 0, 24, 12, 0, 0, 6, 3 };
// Grouped fields: members, written together whenever any of them changed.
static const char *k_sub_names[NP_FIELDS][NP_SUB] = {
    [0] = { "type", "slot" },
    [5] = { "item", "count" },
    [6] = { "hp", "max_hp", "armor" },
    [7] = { "shape", "model", "emissive", "half", "color" },
    [9] = { "light_color", "light_radius", "light_intensity", "light_group" },
};

typedef struct NetProf {
    bool on;
    u64 snapshots;
    struct { u64 sent, bytes, changed; u32 hash[NP_CLIENTS]; bool have[NP_CLIENTS]; } sec[NS_COUNT];
    u64 considered, written, header_bytes;
    struct { u64 sent, bytes, with_base; u64 sub_changed[NP_SUB]; } field[NP_FIELDS];
    struct { u64 count, bytes; } msg[2][32];   // [reliable][type]
} NetProf;
static NetProf NP;

static u32 hash_bytes(const u8 *p, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void netprof_section(int client, NetSection s, const u8 *bytes, u32 len) {
    if (!dg_prof_recording || (unsigned)s >= NS_COUNT) return;
    if (s == NS_HEADER) NP.snapshots++;
    NP.sec[s].sent++;
    NP.sec[s].bytes += len;
    if (client < 0 || client >= NP_CLIENTS) return;
    u32 h = hash_bytes(bytes, len) ^ len;
    if (!NP.sec[s].have[client] || NP.sec[s].hash[client] != h) NP.sec[s].changed++;
    NP.sec[s].hash[client] = h;
    NP.sec[s].have[client] = true;
}

void netprof_entities_considered(int n) { if (dg_prof_recording) NP.considered += (u64)n; }

void netprof_entity(const NetEnt *b, const NetEnt *e, u32 m, u32 bytes) {
    if (!dg_prof_recording) return;
    NP.written++;
    NP.header_bytes += 6;   // id + mask
    u32 known = 6;
    for (int k = 0; k < NP_FIELDS; k++) {
        if (!(m & (1u << k))) continue;
        NP.field[k].sent++;
        u32 sz = k_field_size[k];
        if (k == 8) sz = 1 + (u32)DG_MIN(strlen(e->name), 255u);
        if (k == 12) {
            sz = 1;
            for (int s = 0; s < (int)sizeof(e->attach); s++) if (!b || b->attach[s] != e->attach[s]) sz++;
        }
        NP.field[k].bytes += sz;
        known += sz;
        if (!b) continue;
        NP.field[k].with_base++;
        bool ch[NP_SUB] = { 0 };
        switch (k) {
        case 0: ch[0] = b->type != e->type; ch[1] = b->slot != e->slot; break;
        case 5: ch[0] = b->item != e->item; ch[1] = b->count != e->count; break;
        case 6: ch[0] = b->hp != e->hp; ch[1] = b->max_hp != e->max_hp; ch[2] = b->armor != e->armor; break;
        case 7:
            ch[0] = b->shape != e->shape; ch[1] = b->model != e->model; ch[2] = b->emissive != e->emissive;
            ch[3] = memcmp(&b->half, &e->half, sizeof(vec3)) != 0; ch[4] = b->color != e->color;
            break;
        case 9:
            ch[0] = memcmp(&b->light_color, &e->light_color, sizeof(vec3)) != 0; ch[1] = b->light_radius != e->light_radius;
            ch[2] = b->light_intensity != e->light_intensity; ch[3] = b->light_group != e->light_group;
            break;
        default: break;
        }
        for (int s = 0; s < NP_SUB; s++) NP.field[k].sub_changed[s] += ch[s];
    }
    if ((m & NF_ANIMST) && bytes > known) NP.field[11].bytes += bytes - known;   // the remainder is the anim state
}

void netprof_message(u8 type, u32 bytes, bool reliable) {
    if (!dg_prof_recording) return;
    NP.msg[reliable ? 1 : 0][type & 31].count++;
    NP.msg[reliable ? 1 : 0][type & 31].bytes += bytes;
}

static void cap_begin(int frames) {
    (void)frames;
    memset(&NP, 0, sizeof(NP));
}

static void write_net(FILE *f) {
    fprintf(f, "{\"snapshots\":%llu,\"entities_considered\":%llu,\"entities_written\":%llu,\"entity_header_bytes\":%llu,\"sections\":{", (unsigned long long)NP.snapshots,
            (unsigned long long)NP.considered, (unsigned long long)NP.written, (unsigned long long)NP.header_bytes);
    for (int s = 0; s < NS_COUNT; s++)
        fprintf(f, "%s\"%s\":{\"sent\":%llu,\"bytes\":%llu,\"changed\":%llu}", s ? "," : "", k_section_names[s], (unsigned long long)NP.sec[s].sent,
                (unsigned long long)NP.sec[s].bytes, (unsigned long long)NP.sec[s].changed);
    fprintf(f, "},\"fields\":{");
    for (int k = 0; k < NP_FIELDS; k++) {
        fprintf(f, "%s\"%s\":{\"sent\":%llu,\"bytes\":%llu,\"with_base\":%llu,\"members\":{", k ? "," : "", k_field_names[k], (unsigned long long)NP.field[k].sent,
                (unsigned long long)NP.field[k].bytes, (unsigned long long)NP.field[k].with_base);
        bool first = true;
        for (int s = 0; s < NP_SUB; s++) {
            if (!k_sub_names[k][s]) continue;
            fprintf(f, "%s\"%s\":%llu", first ? "" : ",", k_sub_names[k][s], (unsigned long long)NP.field[k].sub_changed[s]);
            first = false;
        }
        fprintf(f, "}}");
    }
    fprintf(f, "},\"messages\":[");
    bool first = true;
    for (int r = 0; r < 2; r++)
        for (int t = 0; t < 32; t++) {
            if (!NP.msg[r][t].count) continue;
            fprintf(f, "%s{\"type\":%d,\"reliable\":%s,\"count\":%llu,\"bytes\":%llu}", first ? "" : ",", t, r ? "true" : "false", (unsigned long long)NP.msg[r][t].count,
                    (unsigned long long)NP.msg[r][t].bytes);
            first = false;
        }
    fprintf(f, "]}");
}

void netprof_register(void) {
    prof_add_capture_hook(cap_begin, NULL);
    prof_add_section("net", write_net);
}
