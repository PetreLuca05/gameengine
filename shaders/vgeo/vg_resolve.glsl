// vgeo resolve (vgeo_gbuffer.frag, vgeo_lit.frag): this pixel's visibility buffer entry decoded into
// the inputs the shared G-buffer / lit bodies read (gbuffer_body.glsl, lit_body.glsl with
// *_CUSTOM_INPUTS). Perspective-correct barycentrics of the pixel centre on the rasterised triangle
// (raster_view_proj), attributes interpolated in world space; depth is the visibility buffer's own.
// Foliage (Phase 7): positions include the WPO (and the previous frame's WPO for velocity), two-sided
// materials flip back-facing normals, WPO materials' vertex colours are wind weights (not albedo),
// impostor entries sample their atlas.
// glTF maps (docs/PBR.md): the material's map textures + parameters for shaders/pbr_maps.glsl and the
// triangle's tangent frame from its positions and uvs (exact per triangle; no screen derivatives,
// which cross triangle and instance edges in a full-screen resolve).
#include "vgeo/vg_impostor.glsl"
vec3 v_world, v_normal;
vec2 v_uv;
vec4 v_color, v_cur_clip, v_prev_clip;
vec4 g_mat_color, g_mat_params;
uint g_mat_flags;
uint g_tex;                 // texture index of the material (VG_TEXTURES), 0 = none
float g_depth;
bool g_front;               // false: a back face of a two-sided material (the bodies flip the normal)
uvec4 g_pbr_tex;            // metallic-roughness, normal, occlusion, emissive texture indices
uvec4 g_pbr;                // PBR_PARAMS (pbr_maps.glsl)
vec3 g_tangent, g_bitangent;   // world dP/du, dP/dv of the triangle (0: no frame)

// The tangent frame for pbr_normal: the triangle's T / B made orthogonal to the shading normal n
// (directions kept: mirrored uvs stay mirrored).
mat3 vg_tangent_frame(vec3 n) {
    vec3 t = g_tangent - n * dot(n, g_tangent), b = g_bitangent - n * dot(n, g_bitangent);
    if (dot(t, t) < 1e-20 || dot(b, b) < 1e-20) return mat3(vec3(0.0), vec3(0.0), n);
    return mat3(normalize(t), normalize(b), n);
}

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

uint g_vg_iflags = 0u;   // the resolved instance's flags (VGI_*): Receive Shadows off -> MAT_NO_RECEIVE
void vg_resolve_material(VgMaterial mat) {
    g_mat_color = mat.color;
    g_mat_params = vec4(mat.roughness, mat.metallic, mat.emissive, mat.tex_scale);
    // Texture table indices are bounded: a pixel can decode a material record that is not a real
    // material (an index past the table would fault the device; 0 = none / white).
    g_tex = mat.albedo_tex < uint(VG_TEX_COUNT) ? mat.albedo_tex : 0u;
    g_pbr_tex = uvec4(mat.mr_tex, mat.normal_tex, mat.occlusion_tex, mat.emissive_tex);
    g_pbr_tex = mix(uvec4(0u), g_pbr_tex, lessThan(g_pbr_tex, uvec4(uint(VG_TEX_COUNT))));
    g_pbr = uvec4(floatBitsToUint(mat.normal_scale), floatBitsToUint(mat.occlusion_strength), mat.emissive_rgb, floatBitsToUint(mat.alpha_cutoff));
#ifdef VG_TEXTURES
    g_mat_flags = g_tex != 0u ? mat.flags : (mat.flags & ~MAT_TEXTURED);
#else
    g_mat_flags = mat.flags & ~MAT_TEXTURED;
#endif
    g_mat_flags &= ~VGM_FLAGS;
    if ((g_vg_iflags & VGI_NO_RECEIVE) != 0u) g_mat_flags |= MAT_NO_RECEIVE;
    if (g_pbr_tex.x == 0u) g_mat_flags &= ~MAT_MR_TEX;
    if (g_pbr_tex.y == 0u) g_mat_flags &= ~MAT_NORMAL_TEX;
    if (g_pbr_tex.z == 0u) g_mat_flags &= ~MAT_OCCLUSION_TEX;
    if (g_pbr_tex.w == 0u) g_mat_flags &= ~MAT_EMISSIVE_TEX;
}

// Every input defined first, also for empty pixels: the includers discard those, and with the Vulkan
// 1.3 target (SPIR-V 1.6) `discard` is OpDemoteToHelperInvocation, so the lane keeps running the
// G-buffer / lit body as a helper invocation (derivatives). Left undefined, its flags and texture
// indices are register garbage: a map / albedo sample through vg_tex[garbage] can read a descriptor past
// the table and faults the device (VK_ERROR_DEVICE_LOST with a horde on screen). Defaults: no
// texture, no map flags, index 0 (white).
void vg_resolve_defaults() {
    v_world = V.cam_pos.xyz;
    v_normal = vec3(0.0, 1.0, 0.0);
    v_uv = vec2(0.0);
    v_color = vec4(1.0);
    v_cur_clip = v_prev_clip = vec4(0.0, 0.0, 0.0, 1.0);
    g_mat_color = vec4(1.0);
    g_mat_params = vec4(1.0, 0.0, 0.0, 1.0);
    g_mat_flags = 0u;
    g_tex = 0u;
    g_depth = 1.0;
    g_front = true;
    g_pbr_tex = uvec4(0u);
    g_pbr = uvec4(0u);
    g_tangent = g_bitangent = vec3(0.0);
}

bool vg_resolve_pixel() {
    vg_resolve_defaults();
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint w = uint(V.viewport.x);
    uint64_t e = vis[uint(p.y) * w + uint(p.x)];
    if (e == 0ul) return false;
    uint lo = uint(e & 0xFFFFFFFFul), hi = uint(e >> 32);
    g_depth = 1.0 - uintBitsToFloat(hi);
    uvec2 ent = visible[lo >> VG_VIS_TRI_BITS];
    VgXf x = vg_instance_xf(ent.x);
    g_vg_iflags = x.fp >> 16;
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
        g_mat_flags &= ~(MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER | MAT_MR_TEX | MAT_NORMAL_TEX | MAT_OCCLUSION_TEX | MAT_EMISSIVE_TEX);
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
    if (!x.skin && (m.fflags & VGMF_WPO) != 0u && (V.wind_misc.z & VGW_NORMALS) != 0u)   // wind-bent / fluttering normals
        v_normal = normalize(vg_vertex_normal_world(x, c, m, t.x) * b.x + vg_vertex_normal_world(x, c, m, t.y) * b.y + vg_vertex_normal_world(x, c, m, t.z) * b.z);
    else v_normal = normalize(vg_xf_dir(x, n0 * b.x + n1 * b.y + n2 * b.z));
    vec2 u0 = vg_vertex_uv(c, m, t.x), u1 = vg_vertex_uv(c, m, t.y), u2 = vg_vertex_uv(c, m, t.z);
    v_uv = u0 * b.x + u1 * b.y + u2 * b.z;
    v_color = vg_vertex_color(c, m, t.x) * b.x + vg_vertex_color(c, m, t.y) * b.y + vg_vertex_color(c, m, t.z) * b.z;
    v_cur_clip = V.view_proj * vec4(v_world, 1.0);        // unjittered (velocity)
    v_prev_clip = V.prev_view_proj * vec4(prev_world, 1.0);
    // Skinned instances use their slot's material (per character: tint, skin texture), when set.
    uint mi = m.material_base + c.material;
    if (x.slot != 0xFFFFFFFFu && V.caps1.w != 0xFFFFFFFFu) mi = V.caps1.w + x.slot;
    VgMaterial mat = materials[mi];
    if (mat.wpo != WPO_NONE) {                              // wind weights, not albedo
        vec4 wc = v_color;
        v_color = vec4(1.0);
        if (V.wind_misc.w == VGWD_CHANNELS) {                  // wind_debug: R flutter, G phase, B branch as the colour
            vg_resolve_material(mat);
            g_mat_color = vec4(wc.rgb, 1.0);
            g_mat_flags &= ~(MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER | MAT_MR_TEX | MAT_NORMAL_TEX | MAT_OCCLUSION_TEX | MAT_EMISSIVE_TEX);
            if ((mat.flags & VGM_TWO_SIDED) != 0u) g_front = area < 0.0;
            return true;
        }
        if (V.wind_misc.w == VGWD_BEND) {                      // wind_debug: the instance's main bend (black .. red at max)
            vec4 wf = vg_wind_at(x.pos.xz, false);
            vec2 W = wf.xy + (wf.zw - wf.xy) * mat.wind_b.y;
            float bend = min(mat.wpo_params.x * pow(clamp(length(W) / mat.wpo_params.w, 0.0, 1.0), mat.wind_b.z) / max(mat.wind_b.x, 1e-4), 1.0);
            vg_resolve_material(mat);
            g_mat_color = vec4(bend, 0.15 * (1.0 - bend), 0.05, 1.0);
            g_mat_flags &= ~(MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER);
            if ((mat.flags & VGM_TWO_SIDED) != 0u) g_front = area < 0.0;
            return true;
        }
    }
    // Two-sided: Vulkan front faces have a negative signed area here (y down, CCW front).
    if ((mat.flags & VGM_TWO_SIDED) != 0u) g_front = area < 0.0;
    vg_resolve_material(mat);
    if ((g_mat_flags & MAT_NORMAL_TEX) != 0u) {
        vec3 e1 = w1 - w0, e2 = w2 - w0;
        vec2 d1 = u1 - u0, d2 = u2 - u0;
        float r = d1.x * d2.y - d2.x * d1.y;
        if (abs(r) > 1e-12) {
            g_tangent = (e1 * d2.y - e2 * d1.y) / r;
            g_bitangent = (e2 * d1.x - e1 * d2.x) / r;
        }
    }
    return true;
}
