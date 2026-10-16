// vgeo resolve (vgeo_gbuffer.frag, vgeo_lit.frag): this pixel's visibility buffer entry decoded into
// the inputs the shared G-buffer / lit bodies read (gbuffer_body.glsl, lit_body.glsl with
// *_CUSTOM_INPUTS). Perspective-correct barycentrics of the pixel centre on the rasterised triangle
// (raster_view_proj), attributes interpolated in world space; depth is the visibility buffer's own.
// Foliage (Phase 7): positions include the WPO (and the previous frame's WPO for velocity), two-sided
// materials flip back-facing normals, WPO materials' vertex colours are wind weights (not albedo),
// impostor entries sample their atlas.
#include "vgeo/vg_impostor.glsl"
vec3 v_world, v_normal;
vec2 v_uv;
vec4 v_color, v_cur_clip, v_prev_clip;
vec4 g_mat_color, g_mat_params;
uint g_mat_flags;
uint g_tex;                 // texture index of the material (VG_TEXTURES), 0 = none
float g_depth;
bool g_front;               // false: a back face of a two-sided material (the bodies flip the normal)

// Perspective-correct barycentrics of NDC point pn on the triangle with clip positions c0 c1 c2.
vec3 vg_resolve_bary(vec4 c0, vec4 c1, vec4 c2, vec2 pn, out float area) {
    vec2 s0 = c0.xy / c0.w, s1 = c1.xy / c1.w, s2 = c2.xy / c2.w;
    area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    vec3 b;
    if (abs(area) < 1e-12) return vec3(1.0, 0.0, 0.0);
    b.y = ((pn.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (pn.y - s0.y)) / area;
    b.z = ((s1.x - s0.x) * (pn.y - s0.y) - (pn.x - s0.x) * (s1.y - s0.y)) / area;
    b.x = 1.0 - b.y - b.z;
    b /= vec3(c0.w, c1.w, c2.w);
    return b / (b.x + b.y + b.z);
}

void vg_resolve_material(VgMaterial mat) {
    g_mat_color = mat.color;
    g_mat_params = vec4(mat.roughness, mat.metallic, mat.emissive, mat.tex_scale);
    g_tex = mat.albedo_tex;
#ifdef VG_TEXTURES
    g_mat_flags = g_tex != 0u ? mat.flags : (mat.flags & ~MAT_TEXTURED);
#else
    g_mat_flags = mat.flags & ~MAT_TEXTURED;
#endif
    g_mat_flags &= ~VGM_FLAGS;
}

bool vg_resolve_pixel() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint w = uint(V.viewport.x);
    uint64_t e = vis[uint(p.y) * w + uint(p.x)];
    if (e == 0ul) return false;
    uint lo = uint(e & 0xFFFFFFFFul), hi = uint(e >> 32);
    g_depth = 1.0 - uintBitsToFloat(hi);
    g_front = true;
    uvec2 ent = visible[lo >> VG_VIS_TRI_BITS];
    VgXf x = vg_instance_xf(ent.x);
    VgMesh m = meshes[x.mesh];
    vec2 pn = gl_FragCoord.xy * V.viewport.zw * 2.0 - 1.0;
    float area;
    if (ent.y == VG_IMPOSTOR_ENTRY) {
        // Impostor quad: the point on it, the atlas' blended albedo / normal (the material colour and
        // texture are baked in).
        VgImpostorQuad q = vg_impostor_quad(x, m);
        uvec3 t = vg_impostor_tri(lo & ((1u << VG_VIS_TRI_BITS) - 1u));
        vec3 w0 = vg_impostor_corner(q, t.x), w1 = vg_impostor_corner(q, t.y), w2 = vg_impostor_corner(q, t.z);
        vec3 b = vg_resolve_bary(V.raster_view_proj * vec4(w0, 1.0), V.raster_view_proj * vec4(w1, 1.0), V.raster_view_proj * vec4(w2, 1.0), pn, area);
        v_world = w0 * b.x + w1 * b.y + w2 * b.z;
        vec3 nl;
        vec4 a = vg_impostor_sample(x, m, q, v_world, true, nl);
        v_normal = normalize(vg_xf_dir(x, nl));
        v_uv = vec2(0.0);
        v_color = vec4(a.rgb, 1.0);
        v_cur_clip = V.view_proj * vec4(v_world, 1.0);
        v_prev_clip = V.prev_view_proj * vec4(v_world, 1.0);
        vg_resolve_material(materials[m.material_base]);
        g_mat_color = vec4(1.0);
        g_mat_flags &= ~(MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER);
        return true;
    }
    VgClusterRef ref = vg_cluster_ref(ent.y);
    VgClusterData c = vg_cluster(ref.slot, ref.index);
    uvec3 t = vg_triangle(c, lo & ((1u << VG_VIS_TRI_BITS) - 1u));
    // Positions exactly as the rasterisers (vg_vertex_world: skinned instances re-skinned per vertex, WPO).
    vec3 w0 = vg_vertex_world(x, c, m, t.x), w1 = vg_vertex_world(x, c, m, t.y), w2 = vg_vertex_world(x, c, m, t.z);
    vec3 b = vg_resolve_bary(V.raster_view_proj * vec4(w0, 1.0), V.raster_view_proj * vec4(w1, 1.0), V.raster_view_proj * vec4(w2, 1.0), pn, area);
    v_world = w0 * b.x + w1 * b.y + w2 * b.z;
    vec3 n0 = vg_vertex_normal(c, m, t.x), n1 = vg_vertex_normal(c, m, t.y), n2 = vg_vertex_normal(c, m, t.z);
    vec3 prev_world = v_world;
    if (x.skin) {
        // Skinned: normals through each vertex's skin matrix (as skin.vert).
        n0 = mat3(vg_vertex_skin_matrix(x, c, m, t.x, false)) * n0;
        n1 = mat3(vg_vertex_skin_matrix(x, c, m, t.y, false)) * n1;
        n2 = mat3(vg_vertex_skin_matrix(x, c, m, t.z, false)) * n2;
    }
    // Velocity: the previous palette / wind / time with the same barycentrics.
    if (x.skin || (m.fflags & VGMF_WPO) != 0u)
        prev_world = vg_vertex_world_ex(x, c, m, t.x, true) * b.x + vg_vertex_world_ex(x, c, m, t.y, true) * b.y + vg_vertex_world_ex(x, c, m, t.z, true) * b.z;
    v_normal = normalize(vg_xf_dir(x, n0 * b.x + n1 * b.y + n2 * b.z));
    v_uv = vg_vertex_uv(c, m, t.x) * b.x + vg_vertex_uv(c, m, t.y) * b.y + vg_vertex_uv(c, m, t.z) * b.z;
    v_color = vg_vertex_color(c, m, t.x) * b.x + vg_vertex_color(c, m, t.y) * b.y + vg_vertex_color(c, m, t.z) * b.z;
    v_cur_clip = V.view_proj * vec4(v_world, 1.0);        // unjittered (velocity)
    v_prev_clip = V.prev_view_proj * vec4(prev_world, 1.0);
    // Skinned instances use their slot's material (per character: tint, skin texture), when set.
    uint mi = m.material_base + c.material;
    if (x.slot != 0xFFFFFFFFu && V.caps1.w != 0xFFFFFFFFu) mi = V.caps1.w + x.slot;
    VgMaterial mat = materials[mi];
    if (mat.wpo != WPO_NONE) v_color = vec4(1.0);           // wind weights, not albedo
    // Two-sided: Vulkan front faces have a negative signed area here (y down, CCW front).
    if ((mat.flags & VGM_TWO_SIDED) != 0u) g_front = area < 0.0;
    vg_resolve_material(mat);
    return true;
}
