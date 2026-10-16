// vgeo resolve (vgeo_gbuffer.frag, vgeo_lit.frag): this pixel's visibility buffer entry decoded into
// the inputs the shared G-buffer / lit bodies read (gbuffer_body.glsl, lit_body.glsl with
// *_CUSTOM_INPUTS). Perspective-correct barycentrics of the pixel centre on the rasterised triangle
// (raster_view_proj), attributes interpolated in world space; depth is the visibility buffer's own.
vec3 v_world, v_normal;
vec2 v_uv;
vec4 v_color, v_cur_clip, v_prev_clip;
vec4 g_mat_color, g_mat_params;
uint g_mat_flags;
float g_depth;

bool vg_resolve_pixel() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint w = uint(V.viewport.x);
    uint64_t e = vis[uint(p.y) * w + uint(p.x)];
    if (e == 0ul) return false;
    uint lo = uint(e & 0xFFFFFFFFul), hi = uint(e >> 32);
    g_depth = 1.0 - uintBitsToFloat(hi);
    uvec2 ent = visible[lo >> VG_VIS_TRI_BITS];
    VgClusterRef ref = vg_cluster_ref(ent.y);
    VgXf x = vg_instance_xf(ent.x);
    VgMesh m = meshes[x.mesh];
    VgClusterData c = vg_cluster(ref.slot, ref.index);
    uvec3 t = vg_triangle(c, lo & ((1u << VG_VIS_TRI_BITS) - 1u));
    vec3 w0 = vg_xf_point(x, vg_vertex_pos(c, m, t.x)), w1 = vg_xf_point(x, vg_vertex_pos(c, m, t.y)), w2 = vg_xf_point(x, vg_vertex_pos(c, m, t.z));
    vec4 c0 = V.raster_view_proj * vec4(w0, 1.0), c1 = V.raster_view_proj * vec4(w1, 1.0), c2 = V.raster_view_proj * vec4(w2, 1.0);
    // screen-space barycentrics of the pixel centre, then perspective correction (1 / w)
    vec2 pn = gl_FragCoord.xy * V.viewport.zw * 2.0 - 1.0;
    vec2 s0 = c0.xy / c0.w, s1 = c1.xy / c1.w, s2 = c2.xy / c2.w;
    float area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    vec3 b;
    if (abs(area) < 1e-12) b = vec3(1.0, 0.0, 0.0);
    else {
        b.y = ((pn.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (pn.y - s0.y)) / area;
        b.z = ((s1.x - s0.x) * (pn.y - s0.y) - (pn.x - s0.x) * (s1.y - s0.y)) / area;
        b.x = 1.0 - b.y - b.z;
        b /= vec3(c0.w, c1.w, c2.w);
        b /= b.x + b.y + b.z;
    }
    v_world = w0 * b.x + w1 * b.y + w2 * b.z;
    v_normal = normalize(vg_xf_dir(x, vg_vertex_normal(c, m, t.x) * b.x + vg_vertex_normal(c, m, t.y) * b.y + vg_vertex_normal(c, m, t.z) * b.z));
    v_uv = vg_vertex_uv(c, m, t.x) * b.x + vg_vertex_uv(c, m, t.y) * b.y + vg_vertex_uv(c, m, t.z) * b.z;
    v_color = vg_vertex_color(c, m, t.x) * b.x + vg_vertex_color(c, m, t.y) * b.y + vg_vertex_color(c, m, t.z) * b.z;
    v_cur_clip = V.view_proj * vec4(v_world, 1.0);        // unjittered (velocity); static instances
    v_prev_clip = V.prev_view_proj * vec4(v_world, 1.0);
    VgMaterial mat = materials[m.material_base + c.material];
    g_mat_color = mat.color;
    g_mat_params = vec4(mat.roughness, mat.metallic, mat.emissive, mat.tex_scale);
    g_mat_flags = mat.flags & ~MAT_TEXTURED;   // bindless material textures: Phase 5
    return true;
}
