// Impostors (docs/VGEO.md "Foliage support"): an instance past the switch (vg_use_impostor) draws one
// camera-facing quad textured from its mesh's hemi-octahedral atlas (build_vgeo --impostor,
// vgeo_impostor.c): N x N frames, frame (i, j) is an orthographic view of the mesh from direction
// vg_hemi_oct_decode((i, j) / (N - 1)) (mesh space, towards the viewer), half extent impostor_radius
// around the mesh sphere centre, axes vg_impostor_basis. Per pixel the 3 frames nearest to the view
// direction are blended, each sampled where the pixel's point on the quad projects into that frame, so
// the quad's own axes do not matter. Albedo atlas (sRGB, alpha = coverage) at texture m.impostor & 0xFFFF,
// mesh-space normals (xyz * 0.5 + 0.5) at m.impostor >> 16. Include after vg_decode.glsl.
#ifndef VG_IMPOSTOR_GLSL
#define VG_IMPOSTOR_GLSL

vec3 vg_hemi_oct_decode(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    vec3 d = vec3((p.x + p.y) * 0.5, 0.0, (p.x - p.y) * 0.5);
    d.y = 1.0 - abs(d.x) - abs(d.z);
    return normalize(d);
}
vec2 vg_hemi_oct_encode(vec3 d) {
    d.y = max(d.y, 0.0);
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-8);
    return vec2(d.x + d.z, d.x - d.z) * 0.5 + 0.5;
}
// Image axes of a frame seen from direction f (unit, towards the viewer): r right, u up.
void vg_impostor_basis(vec3 f, out vec3 r, out vec3 u) {
    vec3 up = abs(f.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    r = normalize(cross(up, f));
    u = cross(f, r);
}

// The quad: centre (world), half size, and its corners k = 0..3 (-r-u, +r-u, +r+u, -r+u; triangles
// 0 1 2 and 0 2 3), facing the culling camera.
struct VgImpostorQuad { vec3 c, r, u; float h; };
VgImpostorQuad vg_impostor_quad(VgXf x, VgMesh m) {
    VgImpostorQuad q;
    q.c = vg_xf_point(x, m.sphere.xyz);
    q.h = m.impostor_radius * x.pscale;
    vec3 f = V.cam_pos.xyz - q.c;
    f = dot(f, f) > 1e-12 ? normalize(f) : vec3(0.0, 0.0, 1.0);
    vg_impostor_basis(f, q.r, q.u);
    return q;
}
vec3 vg_impostor_corner(VgImpostorQuad q, uint k) {
    float sr = (k == 1u || k == 2u) ? 1.0 : -1.0, su = k >= 2u ? 1.0 : -1.0;
    return q.c + (q.r * sr + q.u * su) * q.h;
}
uvec3 vg_impostor_tri(uint t) { return t == 0u ? uvec3(0u, 1u, 2u) : uvec3(0u, 2u, 3u); }

// The atlas at world point p of the quad: rgb albedo (linear), a coverage; normal: mesh-space normal
// (unnormalised blend). lod from the quad's projected size.
vec4 vg_impostor_sample(VgXf x, VgMesh m, VgImpostorQuad q, vec3 p, bool want_normal, out vec3 normal) {
    normal = vec3(0.0, 1.0, 0.0);
    vec3 l = vg_qrot(vec4(-x.rot.xyz, x.rot.w), p - q.c) / max(x.pscale, 1e-8);   // mesh space, from the centre
    vec3 dl = vg_qrot(vec4(-x.rot.xyz, x.rot.w), V.cam_pos.xyz - q.c);
    uint n = max(m.impostor_grid, 2u);
    vec2 g = vg_hemi_oct_encode(dot(dl, dl) > 1e-12 ? normalize(dl) : vec3(0.0, 1.0, 0.0)) * float(n - 1u);
    vec2 i0 = clamp(floor(g), vec2(0.0), vec2(float(n) - 2.0)), f = g - i0;
    vec2 fr[3];
    vec3 w;
    if (f.x + f.y < 1.0) { fr[0] = i0; fr[1] = i0 + vec2(1.0, 0.0); fr[2] = i0 + vec2(0.0, 1.0); w = vec3(1.0 - f.x - f.y, f.x, f.y); }
    else { fr[0] = i0 + vec2(1.0); fr[1] = i0 + vec2(0.0, 1.0); fr[2] = i0 + vec2(1.0, 0.0); w = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y); }
    uint ta = m.impostor & 0xFFFFu, tn = m.impostor >> 16;
    float size = float(textureSize(vg_tex[nonuniformEXT(ta)], 0).x), tile = size / float(n);
    float dist = max(length(V.cam_pos.xyz - q.c), V.params.z);
    float lod = max(log2(tile / max(2.0 * q.h * V.cam_pos.w / dist, 1e-4)), 0.0);
    float rad = max(m.impostor_radius, 1e-8), hm = 0.5 / tile;
    vec4 acc = vec4(0.0);
    vec3 nacc = vec3(0.0);
    for (int k = 0; k < 3; k++) {
        vec3 fd = vg_hemi_oct_decode(fr[k] / float(n - 1u)), r, u;
        vg_impostor_basis(fd, r, u);
        vec2 t = vec2(0.5 + 0.5 * dot(l, r) / rad, 0.5 - 0.5 * dot(l, u) / rad);
        if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) continue;
        vec2 uv = (fr[k] + clamp(t, vec2(hm), vec2(1.0 - hm))) / float(n);
        vec4 a = textureLod(vg_tex[nonuniformEXT(ta)], uv, lod);
        acc += vec4(a.rgb * a.a, a.a) * w[k];
        if (want_normal) nacc += (textureLod(vg_tex[nonuniformEXT(tn)], uv, lod).xyz * 2.0 - 1.0) * a.a * w[k];
    }
    if (want_normal && dot(nacc, nacc) > 1e-12) normal = nacc;
    return vec4(acc.a > 1e-6 ? acc.rgb / acc.a : vec3(0.0), acc.a);
}
#endif
