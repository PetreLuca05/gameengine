#include "item_view.h"
#include "gameplay/attachments.h"
#include <math.h>
#include <string.h>

static void add(RenderUi3dPart *out, int *n, int max, vec3 c, quat r, vec3 h, vec4 col, u32 shape, float hl, vec3 *mn, vec3 *mx) {
    if (*n >= max) return;
    RenderUi3dPart *p = &out[(*n)++];
    memset(p, 0, sizeof(*p));
    p->center = c;
    p->rot = r;
    p->half = h;
    p->color = col;
    p->shape = shape;
    p->highlight = hl;
    p->metallic = (col.x + col.y + col.z) < 0.9f ? 0.6f : 0.15f;   // dark parts read as metal
    // Bounds: the rotated box's extent.
    vec3 ax = quat_rotate(r, v3(h.x, 0, 0)), ay = quat_rotate(r, v3(0, h.y, 0)), az = quat_rotate(r, v3(0, 0, h.z));
    vec3 e = v3(fabsf(ax.x) + fabsf(ay.x) + fabsf(az.x), fabsf(ax.y) + fabsf(ay.y) + fabsf(az.y), fabsf(ax.z) + fabsf(ay.z) + fabsf(az.z));
    *mn = v3_min(*mn, v3_sub(c, e));
    *mx = v3_max(*mx, v3_add(c, e));
}

int item_view_parts(const ItemDB *db, u16 item, const u8 *att, int highlight_slot, RenderUi3dPart *out, int max, vec3 *mn, vec3 *mx) {
    int n = 0;
    *mn = v3s(1e9f);
    *mx = v3s(-1e9f);
    const ItemDef *it = item_def(db, item);
    const WeaponDef *wd = item_weapon(db, item);
    if (wd) {
        static WeaponDef eff;
        u8 none[ATT_SLOTS] = { 0 };
        attach_weapon_effective(db, wd, att ? att : none, &eff);
        for (int i = 0; i < eff.part_count; i++) add(out, &n, max, eff.parts[i].offset, quat_identity(), eff.parts[i].half, eff.parts[i].color, 0, 0, mn, mx);
        AttachDrawPart parts[ATT_SLOTS * ATT_MAX_PARTS];
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!eff.att_applied[k]) continue;
            u8 one[ATT_SLOTS] = { 0 };
            one[k] = eff.att_applied[k];
            int pc = attach_draw_parts(db, &eff, one, parts, ARRAY_COUNT(parts));
            for (int i = 0; i < pc; i++) {
                const AttachmentDef *ad = attach_def(db, one[k]);
                float em = (ad && ad->slot == ATT_OPTIC && i == pc - 1 && ad->sight == SIGHT_REDDOT) ? 0.0f : 0.0f;
                add(out, &n, max, parts[i].center, parts[i].rot, parts[i].half, parts[i].color, 0, k == highlight_slot ? 0.8f : 0.0f, mn, mx);
                out[n - 1].emissive = em;
            }
        }
        // Socket marker for the highlighted slot (where a dragged attachment goes).
        if (highlight_slot >= 0 && highlight_slot < ATT_SLOTS && wd->att_slots[highlight_slot].present) {
            vec3 sp = wd->att_slots[highlight_slot].socket.t;
            vec3 bmn = *mn, bmx = *mx;
            add(out, &n, max, sp, quat_identity(), v3s(0.012f), v4(1.0f, 0.55f, 0.15f, 1), 0, 1.0f, mn, mx);
            out[n - 1].emissive = 1.5f;
            *mn = bmn;   // the marker doesn't change the framing
            *mx = bmx;
        }
        return n;
    }
    if (it->kind == ITEM_ATTACHMENT) {
        const AttachmentDef *ad = attach_def(db, it->attachment);
        if (ad && ad->part_count) {
            for (int i = 0; i < ad->part_count; i++) add(out, &n, max, ad->parts[i].offset, quat_identity(), ad->parts[i].half, ad->parts[i].color, 0, 0, mn, mx);
            return n;
        }
    }
    // Pickup / held item: its box (world-scale pickup size, so small items still read well).
    vec3 h = it->size;
    u32 shape = it->shape == VIS_CYLINDER ? 1u : 0u;
    add(out, &n, max, v3(0, 0, 0), quat_identity(), h, it->color, shape, 0, mn, mx);
    // A darker band so plain boxes read as objects (label / strap).
    if (shape == 0 && n < max) {
        vec4 band = v4(it->color.x * 0.55f, it->color.y * 0.55f, it->color.z * 0.55f, 1);
        add(out, &n, max, v3(0, 0, 0), quat_identity(), v3(h.x * 1.02f, h.y * 0.28f, h.z * 1.02f), band, 0, 0, mn, mx);
    }
    return n;
}

void item_view_default_cam(const ItemDB *db, u16 item, ItemViewCam *cam) {
    const WeaponDef *wd = item_weapon(db, item);
    const ItemDef *it = item_def(db, item);
    cam->zoom = 1.0f;
    cam->ortho = false;
    if (wd || it->kind == ITEM_ATTACHMENT) {
        // Side view from the right (barrel pointing right), a little from above and the front.
        cam->yaw = 0.42f;
        cam->pitch = -0.32f;
    } else {
        cam->yaw = 0.75f;
        cam->pitch = -0.5f;
    }
}

void item_view_matrices(const ItemViewCam *cam, vec3 mn, vec3 mx, float aspect, mat4 *view, mat4 *proj) {
    vec3 c = v3_scale(v3_add(mn, mx), 0.5f);
    float radius = fmaxf(0.02f, v3_len(v3_scale(v3_sub(mx, mn), 0.5f)));
    // Orbit: yaw 0 = looking from +X (model -Z to the screen right).
    vec3 dir = v3(cosf(cam->pitch) * cosf(cam->yaw), -sinf(cam->pitch), cosf(cam->pitch) * -sinf(cam->yaw));
    dir = v3_norm(dir);
    float fov = 24.0f * DG_DEG2RAD;
    float dist = radius / sinf(fov * 0.5f) / fmaxf(0.2f, cam->zoom);
    vec3 eye = v3_add(c, v3_scale(dir, dist));
    *view = mat4_look_at(eye, c, v3(0, 1, 0));
    if (cam->ortho) {
        // Fit the projected bounds exactly (8 corners in view space), with a margin.
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < 8; i++) {
            vec3 p = v3(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z);
            vec4 v = mat4_mul_v4(*view, v4(p.x, p.y, p.z, 1));
            x0 = fminf(x0, v.x); x1 = fmaxf(x1, v.x); y0 = fminf(y0, v.y); y1 = fmaxf(y1, v.y);
        }
        float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
        if (hw / fmaxf(hh, 1e-4f) > aspect) hh = hw / aspect; else hw = hh * aspect;
        float m = 1.12f / fmaxf(0.2f, cam->zoom);
        *proj = mat4_ortho(cx - hw * m, cx + hw * m, cy - hh * m, cy + hh * m, 0.01f, dist + radius * 4.0f);
    } else {
        *proj = mat4_perspective(fov, aspect, fmaxf(0.005f, dist - radius * 2.0f), dist + radius * 2.0f);
    }
}

bool item_view_project(const mat4 *view, const mat4 *proj, vec3 p, vec2 *out) {
    vec4 v = mat4_mul_v4(mat4_mul(*proj, *view), v4(p.x, p.y, p.z, 1));
    if (v.w <= 1e-5f) return false;
    out->x = (v.x / v.w) * 0.5f + 0.5f;
    out->y = (v.y / v.w) * 0.5f + 0.5f;   // the projection flips Y: +1 is the bottom
    return true;
}
