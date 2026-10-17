// Radial wheel definitions (src/ui/widgets/wheel_def.h). Pure.
#include "wheel_def.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static void str(const cJSON *o, const char *k, char *dst, size_t size) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) dg_strcpy(dst, size, v->valuestring);
}

bool wheel_def_parse(WheelDef *def, const char *json, char *err, int err_size) {
    if (err && err_size) err[0] = 0;
    WheelDef d;
    memset(&d, 0, sizeof(d));
    d.min_slots = 4;
    d.max_slots = 8;
    d.hold_time = -1.0f;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "JSON syntax error"); return false; }
    const char *bad = NULL;
    str(root, "id", d.id, sizeof(d.id));
    str(root, "title", d.title, sizeof(d.title));
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (cJSON_IsString(v)) {
        if (!strcmp(v->valuestring, "inventory")) d.source = WHEEL_INVENTORY;
        else if (!strcmp(v->valuestring, "static")) d.source = WHEEL_STATIC;
        else bad = "\"source\" must be \"static\" or \"inventory\"";
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "filter");
    if (f) {
        str(f, "kind", d.filter_kind, sizeof(d.filter_kind));
        str(f, "tag", d.filter_tag, sizeof(d.filter_tag));
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "min_slots"))) d.min_slots = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "max_slots"))) d.max_slots = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "hold_time"))) d.hold_time = (float)v->valuedouble;
    if (!d.id[0]) bad = "missing \"id\"";
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    const cJSON *e;
    cJSON_ArrayForEach(e, slots) {
        if (bad) break;
        if (d.slot_count >= RADIAL_MAX_SLOTS) { bad = "more than 12 slots"; break; }
        WheelSlotDef *s = &d.slots[d.slot_count];
        s->color = -1;
        if (cJSON_IsString(e)) { dg_strcpy(s->id, sizeof(s->id), e->valuestring); d.slot_count++; continue; }   // "order": bare ids
        str(e, "id", s->id, sizeof(s->id));
        str(e, "label", s->label, sizeof(s->label));
        str(e, "icon", s->icon, sizeof(s->icon));
        str(e, "desc", s->desc, sizeof(s->desc));
        str(e, "sub", s->sub, sizeof(s->sub));
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "color"))) s->color = DG_CLAMP(v->valueint, 0, 7);
        if (!s->id[0]) { bad = "a slot has no \"id\""; break; }
        if (!s->label[0]) dg_strcpy(s->label, sizeof(s->label), s->id);
        d.slot_count++;
    }
    if (!bad && d.source == WHEEL_STATIC && d.slot_count < RADIAL_MIN_SLOTS) bad = "a static wheel needs 2..12 slots";
    if (!bad && (d.min_slots < RADIAL_MIN_SLOTS || d.max_slots > RADIAL_MAX_SLOTS || d.min_slots > d.max_slots)) bad = "\"min_slots\" / \"max_slots\" must be within 2..12";
    if (!bad) {
        char def_id[32] = "";
        str(root, "default", def_id, sizeof(def_id));
        d.default_slot = def_id[0] ? wheel_slot_index(&d, def_id) : -1;
        if (def_id[0] && d.default_slot < 0) bad = "\"default\" names no slot";
    }
    cJSON_Delete(root);
    if (bad) {
        if (err) snprintf(err, (size_t)err_size, "%s%s%s", d.id[0] ? d.id : "", d.id[0] ? ": " : "", bad);
        return false;
    }
    *def = d;
    return true;
}

const WheelDef *wheel_lib_find(const WheelLib *lib, const char *id) {
    for (int i = 0; i < lib->count; i++) if (!strcmp(lib->defs[i].id, id)) return &lib->defs[i];
    return NULL;
}

bool wheel_lib_add(WheelLib *lib, const WheelDef *def) {
    for (int i = 0; i < lib->count; i++) if (!strcmp(lib->defs[i].id, def->id)) { lib->defs[i] = *def; return true; }
    if (lib->count >= WHEEL_MAX_DEFS) return false;
    lib->defs[lib->count++] = *def;
    return true;
}

int wheel_slot_index(const WheelDef *def, const char *id) {
    for (int i = 0; i < def->slot_count; i++) if (!strcmp(def->slots[i].id, id)) return i;
    return -1;
}

int wheel_def_order(const WheelDef *def, const char *item_id) { return wheel_slot_index(def, item_id); }
