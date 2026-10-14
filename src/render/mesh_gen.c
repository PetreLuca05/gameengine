#include "mesh_gen.h"
#include <stdlib.h>
#include <math.h>

void mb_init(MeshBuilder *mb) { memset(mb, 0, sizeof(*mb)); }

void mb_free(MeshBuilder *mb) {
    free(mb->verts);
    free(mb->indices);
    memset(mb, 0, sizeof(*mb));
}

void mb_reset(MeshBuilder *mb) { mb->vcount = mb->icount = 0; }

u32 mb_vertex(MeshBuilder *mb, vec3 p, vec3 n, vec2 uv, u32 color) {
    if (mb->vcount == mb->vcap) {
        mb->vcap = mb->vcap ? mb->vcap * 2 : 256;
        mb->verts = (Vertex *)realloc(mb->verts, sizeof(Vertex) * mb->vcap);
    }
    Vertex *v = &mb->verts[mb->vcount];
    v->pos = p;
    v->normal = n;
    v->uv = uv;
    v->color = color;
    return mb->vcount++;
}

void mb_tri(MeshBuilder *mb, u32 a, u32 b, u32 c) {
    if (mb->icount + 3 > mb->icap) {
        mb->icap = mb->icap ? mb->icap * 2 : 512;
        mb->indices = (u32 *)realloc(mb->indices, sizeof(u32) * mb->icap);
    }
    mb->indices[mb->icount++] = a;
    mb->indices[mb->icount++] = b;
    mb->indices[mb->icount++] = c;
}

static vec2 face_uv(vec3 p, vec3 n) {
    vec3 an = v3(fabsf(n.x), fabsf(n.y), fabsf(n.z));
    if (an.y >= an.x && an.y >= an.z) return v2(p.x, p.z);
    if (an.x >= an.z) return v2(p.z, p.y);
    return v2(p.x, p.y);
}

void mb_quad(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, u32 color) {
    u32 i0 = mb_vertex(mb, a, n, face_uv(a, n), color);
    u32 i1 = mb_vertex(mb, b, n, face_uv(b, n), color);
    u32 i2 = mb_vertex(mb, c, n, face_uv(c, n), color);
    u32 i3 = mb_vertex(mb, d, n, face_uv(d, n), color);
    mb_tri(mb, i0, i1, i2);
    mb_tri(mb, i0, i2, i3);
}

void mb_box(MeshBuilder *mb, vec3 c, vec3 h, u32 color) {
    float x0 = c.x - h.x, x1 = c.x + h.x, y0 = c.y - h.y, y1 = c.y + h.y, z0 = c.z - h.z, z1 = c.z + h.z;
    mb_quad(mb, v3(x0, y0, z1), v3(x1, y0, z1), v3(x1, y1, z1), v3(x0, y1, z1), v3(0, 0, 1), color);   // +Z
    mb_quad(mb, v3(x1, y0, z0), v3(x0, y0, z0), v3(x0, y1, z0), v3(x1, y1, z0), v3(0, 0, -1), color);  // -Z
    mb_quad(mb, v3(x1, y0, z1), v3(x1, y0, z0), v3(x1, y1, z0), v3(x1, y1, z1), v3(1, 0, 0), color);   // +X
    mb_quad(mb, v3(x0, y0, z0), v3(x0, y0, z1), v3(x0, y1, z1), v3(x0, y1, z0), v3(-1, 0, 0), color);  // -X
    mb_quad(mb, v3(x0, y1, z1), v3(x1, y1, z1), v3(x1, y1, z0), v3(x0, y1, z0), v3(0, 1, 0), color);   // +Y
    mb_quad(mb, v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y0, z1), v3(x0, y0, z1), v3(0, -1, 0), color);  // -Y
}

void mb_box_transformed(MeshBuilder *mb, mat4 xf, vec3 h, u32 color) {
    u32 start = mb->vcount;
    mb_box(mb, v3(0, 0, 0), h, color);
    for (u32 i = start; i < mb->vcount; i++) {
        Vertex *v = &mb->verts[i];
        v->pos = mat4_mul_point(xf, v->pos);
        v->normal = v3_norm(mat4_mul_dir(xf, v->normal));
        v->uv = face_uv(v->pos, v->normal);
    }
}

void mb_wedge(MeshBuilder *mb, vec3 b, float hx, float hz, float height, u32 color) {
    float x0 = b.x - hx, x1 = b.x + hx, z0 = b.z - hz, z1 = b.z + hz, y0 = b.y, y1 = b.y + height;
    vec3 slope_n = v3_norm(v3(0, 2.0f * hz, -height));
    mb_quad(mb, v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y1, z1), v3(x0, y1, z1), slope_n, color);       // slope
    mb_quad(mb, v3(x1, y0, z1), v3(x0, y0, z1), v3(x0, y1, z1), v3(x1, y1, z1), v3(0, 0, 1), color);   // back (+Z)
    mb_quad(mb, v3(x0, y0, z0), v3(x0, y0, z1), v3(x1, y0, z1), v3(x1, y0, z0), v3(0, -1, 0), color);  // bottom
    // side triangles
    vec3 nx1 = v3(1, 0, 0), nx0 = v3(-1, 0, 0);
    u32 a = mb_vertex(mb, v3(x1, y0, z0), nx1, face_uv(v3(x1, y0, z0), nx1), color);
    u32 bb = mb_vertex(mb, v3(x1, y0, z1), nx1, face_uv(v3(x1, y0, z1), nx1), color);
    u32 c = mb_vertex(mb, v3(x1, y1, z1), nx1, face_uv(v3(x1, y1, z1), nx1), color);
    mb_tri(mb, a, bb, c);
    a = mb_vertex(mb, v3(x0, y0, z1), nx0, face_uv(v3(x0, y0, z1), nx0), color);
    bb = mb_vertex(mb, v3(x0, y0, z0), nx0, face_uv(v3(x0, y0, z0), nx0), color);
    c = mb_vertex(mb, v3(x0, y1, z1), nx0, face_uv(v3(x0, y1, z1), nx0), color);
    mb_tri(mb, a, bb, c);
}

void mb_cylinder(MeshBuilder *mb, vec3 c, float r, float hh, int seg, u32 color) {
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / seg * 2.0f * DG_PI, a1 = (float)(i + 1) / seg * 2.0f * DG_PI;
        vec3 n0 = v3(cosf(a0), 0, sinf(a0)), n1 = v3(cosf(a1), 0, sinf(a1));
        vec3 p0 = v3_add(c, v3(n0.x * r, -hh, n0.z * r)), p1 = v3_add(c, v3(n1.x * r, -hh, n1.z * r));
        vec3 p2 = v3_add(c, v3(n1.x * r, hh, n1.z * r)), p3 = v3_add(c, v3(n0.x * r, hh, n0.z * r));
        float u0 = (float)i / seg * 2.0f * DG_PI * r, u1 = (float)(i + 1) / seg * 2.0f * DG_PI * r;
        u32 i0 = mb_vertex(mb, p0, n0, v2(u0, -hh), color), i1 = mb_vertex(mb, p1, n1, v2(u1, -hh), color);
        u32 i2 = mb_vertex(mb, p2, n1, v2(u1, hh), color), i3 = mb_vertex(mb, p3, n0, v2(u0, hh), color);
        mb_tri(mb, i0, i2, i1);
        mb_tri(mb, i0, i3, i2);
        // caps
        vec3 up = v3(0, 1, 0), dn = v3(0, -1, 0);
        u32 ct = mb_vertex(mb, v3_add(c, v3(0, hh, 0)), up, v2(c.x, c.z), color);
        u32 t0 = mb_vertex(mb, p3, up, v2(p3.x, p3.z), color), t1 = mb_vertex(mb, p2, up, v2(p2.x, p2.z), color);
        mb_tri(mb, ct, t1, t0);
        u32 cb = mb_vertex(mb, v3_add(c, v3(0, -hh, 0)), dn, v2(c.x, c.z), color);
        u32 b0 = mb_vertex(mb, p0, dn, v2(p0.x, p0.z), color), b1 = mb_vertex(mb, p1, dn, v2(p1.x, p1.z), color);
        mb_tri(mb, cb, b0, b1);
    }
}

static void sphere_part(MeshBuilder *mb, vec3 c, float r, int rings, int seg, u32 color, float y_offset_top, float y_offset_bottom) {
    u32 base = mb->vcount;
    for (int ri = 0; ri <= rings; ri++) {
        float v = (float)ri / rings;
        float phi = v * DG_PI;
        float y = cosf(phi), sr = sinf(phi);
        float off = y >= 0 ? y_offset_top : y_offset_bottom;
        for (int si = 0; si <= seg; si++) {
            float u = (float)si / seg;
            float th = u * 2.0f * DG_PI;
            vec3 n = v3(sr * cosf(th), y, sr * sinf(th));
            vec3 p = v3_add(c, v3(n.x * r, n.y * r + off, n.z * r));
            mb_vertex(mb, p, n, v2(u * 2.0f * DG_PI * r, v * DG_PI * r), color);
        }
    }
    for (int ri = 0; ri < rings; ri++)
        for (int si = 0; si < seg; si++) {
            u32 a = base + (u32)(ri * (seg + 1) + si), b = a + (u32)(seg + 1);
            mb_tri(mb, a, a + 1, b);
            mb_tri(mb, b, a + 1, b + 1);
        }
}

void mb_sphere(MeshBuilder *mb, vec3 c, float r, int rings, int seg, u32 color) { sphere_part(mb, c, r, rings, seg, color, 0, 0); }

void mb_capsule(MeshBuilder *mb, vec3 c, float r, float hh, int rings, int seg, u32 color) {
    // Even ring count guarantees an equator row to split the hemispheres at.
    if (rings % 2) rings++;
    sphere_part(mb, c, r, rings, seg, color, hh, -hh);
    mb_cylinder(mb, c, r, hh, seg, color);
}

MeshId mb_upload(MeshBuilder *mb) { return render_mesh_create(mb->verts, mb->vcount, mb->indices, mb->icount); }

void basic_meshes_create(BasicMeshes *out) {
    MeshBuilder mb;
    mb_init(&mb);
    u32 white = 0xFFFFFFFFu;
    mb_box(&mb, v3(0, 0, 0), v3s(0.5f), white);
    out->cube = mb_upload(&mb);
    mb_reset(&mb);
    mb_sphere(&mb, v3(0, 0, 0), 0.5f, 12, 20, white);
    out->sphere = mb_upload(&mb);
    mb_reset(&mb);
    mb_cylinder(&mb, v3(0, 0, 0), 0.5f, 0.5f, 20, white);
    out->cylinder = mb_upload(&mb);
    mb_reset(&mb);
    mb_capsule(&mb, v3(0, 0, 0), 0.5f, 0.5f, 12, 20, white);
    out->capsule = mb_upload(&mb);
    mb_reset(&mb);
    mb_quad(&mb, v3(-0.5f, -0.5f, 0), v3(0.5f, -0.5f, 0), v3(0.5f, 0.5f, 0), v3(-0.5f, 0.5f, 0), v3(0, 0, 1), white);
    out->quad = mb_upload(&mb);
    mb_free(&mb);
}
