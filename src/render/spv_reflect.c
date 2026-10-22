// Minimal SPIR-V reflection: descriptor bindings of one set (spv_reflect.h).
#include "spv_reflect.h"
#include <stdlib.h>
#include <string.h>

enum {
    OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
};
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

typedef struct SpvId {
    u16 op;
    u8 block, buffer_block, has_binding, has_set;
    u32 a, b;           // pointer: storage class, pointee; image: sampled (1 / 2); sampled image / array: element
    u32 binding, set;
} SpvId;

int spv_reflect_bindings(const u32 *code, size_t words, u32 set, SpvBinding *out, int max) {
    if (!code || words < 5 || code[0] != 0x07230203u) return -1;
    u32 bound = code[3];
    if (!bound || bound > (1u << 22)) return -1;
    SpvId *ids = (SpvId *)calloc(bound, sizeof(SpvId));
    if (!ids) return -1;
    for (size_t i = 5; i < words;) {
        u32 n = code[i] >> 16, op = code[i] & 0xFFFFu;
        if (!n || i + n > words) break;
        const u32 *w = code + i;
        if (op == OP_DECORATE && n >= 3 && w[1] < bound) {
            SpvId *d = &ids[w[1]];
            if (w[2] == DEC_BINDING && n >= 4) d->binding = w[3], d->has_binding = 1;
            else if (w[2] == DEC_SET && n >= 4) d->set = w[3], d->has_set = 1;
            else if (w[2] == DEC_BLOCK) d->block = 1;
            else if (w[2] == DEC_BUFFER_BLOCK) d->buffer_block = 1;
        } else if ((op == OP_TYPE_IMAGE && n >= 9) || op == OP_TYPE_SAMPLER || op == OP_TYPE_STRUCT) {
            if (w[1] < bound) {
                ids[w[1]].op = (u16)op;
                if (op == OP_TYPE_IMAGE) ids[w[1]].a = w[7];
            }
        } else if ((op == OP_TYPE_SAMPLED_IMAGE || op == OP_TYPE_ARRAY || op == OP_TYPE_RUNTIME_ARRAY) && n >= 3) {
            if (w[1] < bound) ids[w[1]].op = (u16)op, ids[w[1]].b = w[2];
        } else if (op == OP_TYPE_POINTER && n >= 4) {
            if (w[1] < bound) ids[w[1]].op = (u16)op, ids[w[1]].a = w[2], ids[w[1]].b = w[3];
        } else if (op == OP_VARIABLE && n >= 4) {
            if (w[2] < bound) ids[w[2]].op = (u16)op, ids[w[2]].a = w[1], ids[w[2]].b = w[3];
        }
        i += n;
    }
    int count = 0;
    for (u32 v = 0; v < bound && count < max; v++) {
        const SpvId *var = &ids[v];
        if (var->op != OP_VARIABLE || !var->has_binding || !var->has_set || var->set != set) continue;
        if (var->a >= bound || ids[var->a].op != OP_TYPE_POINTER) continue;
        u32 sc = ids[var->a].a, t = ids[var->a].b;
        for (int k = 0; k < 4 && t < bound && (ids[t].op == OP_TYPE_ARRAY || ids[t].op == OP_TYPE_RUNTIME_ARRAY); k++) t = ids[t].b;
        if (t >= bound) continue;
        const SpvId *ty = &ids[t];
        SpvDescKind kind;
        if (sc == SC_STORAGE_BUFFER) kind = SPV_DESC_STORAGE_BUFFER;
        else if (sc == SC_UNIFORM) kind = ty->buffer_block ? SPV_DESC_STORAGE_BUFFER : SPV_DESC_UNIFORM_BUFFER;
        else if (sc != SC_UNIFORM_CONSTANT) continue;
        else if (ty->op == OP_TYPE_SAMPLER) kind = SPV_DESC_SAMPLER;
        else if (ty->op == OP_TYPE_SAMPLED_IMAGE) kind = SPV_DESC_COMBINED;
        else if (ty->op == OP_TYPE_IMAGE) kind = ty->a == 2 ? SPV_DESC_STORAGE_IMAGE : SPV_DESC_SAMPLED_IMAGE;
        else continue;
        out[count].binding = var->binding;
        out[count].kind = kind;
        count++;
    }
    free(ids);
    // insertion sort by binding (a handful of entries)
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && out[j - 1].binding > out[j].binding; j--) {
            SpvBinding t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return count;
}
