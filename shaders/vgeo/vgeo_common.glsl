// Virtualized geometry: GLSL mirror of src/render/vgeo/vgeo_gpu.h and the shared cull helpers.
// Change a struct here and in vgeo_gpu.h together (sizes are asserted on the C side).
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef VG_VIS
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require
#endif

struct VgViewGpu {
    mat4 view_proj;
    mat4 prev_view_proj;
    vec4 planes[6];
    vec4 cam_pos;            // xyz, w = proj_scale
    vec4 params;             // x error_px, y sw_px, z znear, w flags (uint bits)
    vec4 viewport;           // w, h, 1 / w, 1 / h
    vec4 hiz_prev;           // w, h, mips, valid
    vec4 hiz_cur;
    uvec4 counts;            // cell_count, instance_count, max_depth, frame (streaming stamp)
    uvec4 caps0;             // nodes (per region), groups (per region), visible (per list), occluded
    uvec4 caps1;             // cells (per region), instances, requests, slot material base (skinned instances: materials[base + slot], ~0 none)
    mat4 raster_view_proj;   // the view the visible clusters are rasterised with (main view: jittered)
    vec4 wind;               // foliage (Phase 7): the wind field outside its tile: instant x, z, settled bend x, z (m/s)
    vec4 wind_prev;          // last frame's (velocity)
    vec4 foliage;            // time, previous time, r_foliage_density, r_vgeo_impostor_px
    vec4 disp;               // displacement buffer: texel 0 world x, z, metres per texel, valid
    uvec4 misc;              // x frame index (dither rotation), y displacement texture index, z / w float bits: fol_fade_max, fol_shadow_max
    vec4 wind_tile;          // wind field tile: texel 0 world x, z, metres per texel, valid
    vec4 wind_tile_prev;     // last frame's
    uvec4 wind_misc;         // x tile texture, y last frame's tile texture, z layers (VGW_*), w debug view (VGWD_*)
    uvec4 shadow_misc;       // x shadow views: the light's culling mask (0 = every layer)
};
struct VgMaterial {
    vec4 color; float roughness, metallic, emissive, tex_scale; uint flags; uint albedo_tex; uint mask_tex; float alpha_cutoff;
    uint wpo; float wpo_max, fade_start, fade_end; vec4 wpo_params; float shadow_max_dist;
    uint mr_tex, normal_tex, occlusion_tex, emissive_tex; float normal_scale, occlusion_strength; uint emissive_rgb;   // glTF maps (shaders/pbr_maps.glsl)
    vec4 wind_a, wind_b;     // wind_params[8]: height, main / detail / flutter Hz | tan(max bend), settle mix, response, 0
};
struct VgNode { vec4 sphere; float max_parent_error; uint first; uint count_flags; uint pad; };
struct VgGroup { vec4 lod; float parent_error; uint page; uint first_count; uint level; };
struct VgMesh {
    vec4 sphere; uint root_node, node_base, group_base, page_base; uint page_count, flags, material_base, impostor;
    float max_wpo, q_step, fade_start, fade_end; float shadow_max_dist; uint fflags, impostor_grid; float impostor_radius;
};
struct VgInstance { vec3 pos; uint rot; uint scale_mesh; uint flags_phase; uint extra; uint cell; };
struct VgCell { vec3 mn; uint first; vec3 mx; uint count; };
struct VgPaletteSlot { uint first, prev_first, bone_count, pad0; float max_scale, k0, k1, pad1; };

#define VGV_NO_FRUSTUM 1u
#define VGV_NO_CONE 2u
#define VGV_NO_OCCLUSION 4u
#define VGV_ORTHO 8u
#define VGV_TWO_PASS 16u
#define VGV_NO_SKINNED 32u
#define VGN_LEAF 1u
#define VGC_TWO_SIDED 1u
#define VGC_NO_CONE 2u
#define VG_PAGE_WORDS 32768u
#define VG_CLUSTER_BITS 12u

// Counters (VgCounters): args[8] (uvec4) then n[32].
#define VGA_INST 0
#define VGA_NODE 1
#define VGA_GROUP 2
#define VGA_INSTLIST 3
#define VGA_OCCL 4
#define VGA_DRAW_HW 5
#define VGA_SW 6
#define VGA_MESH 7
#define VG_VIS_TRI_BITS 7u
#define VGF_SKINNED 1u
#define VGF_HAS_COLOR 4u
#define VGI_SKINNED 2u
#define VGI_NO_CAST 8u          // Unity-style renderer flags (docs/SHADOWS_UNITY_MODEL.md)
#define VGI_SHADOWS_ONLY 16u
#define VGI_NO_RECEIVE 32u
#define VGN_CELLS_VIS 0
#define VGN_CELLS_OCC 1
#define VGN_INST_VIS 2
#define VGN_INST_OCC 3
#define VGN_NODEQ0 4
#define VGN_NODES_VISITED 7
#define VGN_NODES_OCC 8
#define VGN_GROUPS0 9
#define VGN_CLUSTERS_SELECTED 11
#define VGN_HW 12
#define VGN_SW 13
#define VGN_HW_P1 14
#define VGN_SW_P1 15
#define VGN_CL_OCC 16
#define VGN_PAGE_REQ 17
#define VGN_OVERFLOW 18
#define VGN_TRIS 19
#define VGN_INST_P2 20
#define VGN_CL_P2 21
#define VGN_CELLS_P2 22
#define VGN_IMPOSTORS 23
#define VGN_INST_FADED 24
#define VG_IMPOSTOR_ENTRY 0xFFFFFFFFu
#define VG_CELL_FOLIAGE 0x80000000u
#define VG_CELL_COUNT 0x7FFFFFFFu
// Foliage (Phase 7, vgeo_gpu.h): material flags, mesh flags, WPO snippets.
#define VGM_MASKED (1u << 24)
#define VGM_TWO_SIDED (1u << 25)
#define VGM_DITHER_FADE (1u << 26)
#define VGM_FOLIAGE (1u << 27)
#define VGM_FLAGS (VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE | VGM_FOLIAGE)
#define VGMF_FOLIAGE 1u
#define VGMF_MASKED 2u
#define VGMF_TWO_SIDED 4u
#define VGMF_DITHER 8u
#define VGMF_WPO 16u
#define WPO_NONE 0u
#define WPO_GRASS 1u
#define WPO_TREE 2u
#define WPO_BUSH 3u
#define WPO_DISPLACE 256u
#define VGW_MAIN 1u
#define VGW_DETAIL 2u
#define VGW_FLUTTER 4u
#define VGW_NORMALS 8u
#define VGWD_NONE 0u
#define VGWD_CHANNELS 1u
#define VGWD_BEND 2u
#define VG_DISPLACE_MAX 0.6
// Material / impostor / displacement textures (vgeo_tex.h): binding 23, renderer texture ids [0, 1024),
// the vgeo module's own [1024, 1152).
#define VG_TEX_COUNT 1152

// Descriptor set of the vgeo bindings (the resolve passes put it after the renderer's sets) and the
// push constants: VG_NO_PUSH for shaders with their own push block (the view is then VG_VIEW_INDEX).
#ifndef VG_SET
#define VG_SET 0
#endif
#ifndef VG_NO_PUSH
layout(push_constant, scalar) uniform Push { uint pass; uint in_region; uint out_region; uint view; uint a[8]; } pc;
#endif

layout(set = VG_SET, binding = 0, scalar) readonly buffer Views { VgViewGpu views[]; };
layout(set = VG_SET, binding = 1, scalar) readonly buffer Meshes { VgMesh meshes[]; };
layout(set = VG_SET, binding = 2, scalar) readonly buffer Nodes { VgNode nodes[]; };
layout(set = VG_SET, binding = 3, scalar) readonly buffer Groups { VgGroup groups[]; };
layout(set = VG_SET, binding = 4) readonly buffer PageTable { uint page_table[]; };
layout(set = VG_SET, binding = 5) readonly buffer Pool { uint pool[]; };
layout(set = VG_SET, binding = 6, scalar) readonly buffer Instances { VgInstance instances[]; };
layout(set = VG_SET, binding = 7, scalar) readonly buffer Cells { VgCell cells[]; };
layout(set = VG_SET, binding = 8) coherent buffer Counters { uvec4 args[8]; uint n[32]; } ctr;
layout(set = VG_SET, binding = 9) buffer CellList { uint cell_list[]; };
layout(set = VG_SET, binding = 10) buffer InstList { uint inst_list[]; };
layout(set = VG_SET, binding = 11) buffer NodeQueue { uvec2 node_queue[]; };
layout(set = VG_SET, binding = 12) buffer GroupItems { uvec2 group_items[]; };
layout(set = VG_SET, binding = 13) buffer Visible { uvec2 visible[]; };
layout(set = VG_SET, binding = 14) buffer Occluded { uvec2 occluded[]; };
layout(set = VG_SET, binding = 15) buffer Requests { uvec2 requests[]; };
layout(set = VG_SET, binding = 16) uniform sampler2D hiz_prev;
layout(set = VG_SET, binding = 17) uniform sampler2D hiz_cur;
layout(set = VG_SET, binding = 19, scalar) readonly buffer Materials { VgMaterial materials[]; };
// Streaming: per pool slot, the last frame (counts.w) a leaf emitted one of its groups (LRU).
layout(set = VG_SET, binding = 20) buffer PageStamps { uint page_stamp[]; };
// Skinned instances (vgeo_skin.h): palettes (bind space -> instance space) and the slot table.
layout(set = VG_SET, binding = 21) readonly buffer Palettes { mat4 palette[]; };
layout(set = VG_SET, binding = 22, scalar) readonly buffer PaletteSlots { VgPaletteSlot palette_slots[]; };
layout(set = VG_SET, binding = 23) uniform sampler2D vg_tex[VG_TEX_COUNT];
// Visibility buffer (binding 18, see vgeo_gpu.h): define VG_VIS before including (needs int64).
#ifdef VG_VIS
layout(set = VG_SET, binding = 18) coherent buffer VisBuffer { uint64_t vis[]; };
#endif

#ifndef VG_NO_PUSH
#define V views[pc.view]
#else
#define V views[VG_VIEW_INDEX]
#endif

// ------------------------------------------------------------------ instances

vec4 vg_quat_decode(uint e) {
    uint big = e >> 30;
    vec3 v = (vec3(float(e & 1023u), float((e >> 10) & 1023u), float((e >> 20) & 1023u)) / 1023.0 * 2.0 - 1.0) * 0.70710678;
    float w = sqrt(max(0.0, 1.0 - dot(v, v)));
    if (big == 0u) return vec4(w, v.x, v.y, v.z);
    if (big == 1u) return vec4(v.x, w, v.y, v.z);
    if (big == 2u) return vec4(v.x, v.y, w, v.z);
    return vec4(v.x, v.y, v.z, w);
}
vec3 vg_qrot(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
// scale: the error / bounds scale (skinned: instance scale x the slot's max bone scale); pscale: the
// instance's own scale (positions). Skinned (skin): first / prev_first = the slot's palette regions,
// root = palette[first] (bone 0), skin_s / skin_k: the sphere transform of vg_world_sphere. wpo: the
// mesh's max world-position offset (m, added to every world sphere); fp: VgInstance.flags_phase.
struct VgXf { vec3 pos; vec4 rot; float scale; uint mesh; float pscale; bool skin, live; uint first, prev_first; mat4 root; float skin_s, skin_k; uint slot; float wpo; uint fp; };
VgXf vg_instance_xf(uint i) {
    VgInstance in_ = instances[i];
    VgXf x;
    x.pos = in_.pos;
    x.rot = vg_quat_decode(in_.rot);
    x.pscale = unpackHalf2x16(in_.scale_mesh & 0xFFFFu).x;
    x.scale = x.pscale;
    x.mesh = in_.scale_mesh >> 16;
    x.wpo = meshes[x.mesh].max_wpo * max(x.pscale, 1.0);   // vg_wpo_margin
    x.fp = in_.flags_phase;
    x.skin = false;
    x.live = true;
    x.first = x.prev_first = 0u;
    x.root = mat4(1.0);
    x.skin_s = 1.0;
    x.skin_k = 0.0;
    x.slot = 0xFFFFFFFFu;
    if ((in_.flags_phase >> 16 & VGI_SKINNED) != 0u) {
        x.slot = in_.extra;
        VgMesh m = meshes[x.mesh];
        if ((m.flags & VGF_SKINNED) != 0u) {
            VgPaletteSlot s = palette_slots[in_.extra];
            x.skin = true;
            x.live = s.bone_count != 0u;
            x.first = s.first;
            x.prev_first = s.prev_first;
            x.root = palette[s.first];
            x.skin_s = s.max_scale;
            x.skin_k = s.k0 + s.k1 * (length(m.sphere.xyz) + m.sphere.w);
            x.scale = x.pscale * s.max_scale;
        }
    }
    return x;
}
// Instance-space sphere -> world.
vec4 vg_instance_sphere(VgXf x, vec4 s) { return vec4(x.pos + vg_qrot(x.rot, s.xyz * x.pscale), s.w * x.pscale); }
// Mesh-space sphere -> world. Skinned: (P_0 c, r max_scale + k) first, which contains the skinned
// position of every bind-space point of the sphere (|P_b v - P_0 v| <= k for every bone b), and
// keeps nested spheres nested (the DAG cut stays monotonic). Foliage: + the mesh's declared max
// world-position offset (WPO), which keeps them nested too. CPU mirror: vgeo_xf_sphere.
vec4 vg_world_sphere(VgXf x, vec4 s) {
    if (x.skin) s = vec4((x.root * vec4(s.xyz, 1.0)).xyz, s.w * x.skin_s + x.skin_k);
    vec4 w = vg_instance_sphere(x, s);
    w.w += x.wpo;
    return w;
}

// ------------------------------------------------------------------ foliage instances (Phase 7)

// Fade alpha of a foliage instance at distance dist from the camera: 0 = culled, 1 = opaque, in between
// dithered (VGM_DITHER_FADE). Kept while its fixed density hash h < r_foliage_density x fade(dist), so
// the kept set is stable for a static camera and shrinks monotonically with distance; the last 1/16
// of the threshold is the dither band. Static meshes: 1. CPU mirror: vgeo_foliage_alpha.
float vg_foliage_alpha(VgMesh m, uint fp, float dist) {
    if ((m.fflags & VGMF_FOLIAGE) == 0u) return 1.0;
    float t = V.foliage.z;
    if (m.fade_end > m.fade_start) t *= clamp((m.fade_end - dist) / (m.fade_end - m.fade_start), 0.0, 1.0);
    float h = (float((fp >> 8) & 0xFFu) + 0.5) * (0.9375 / 256.0);
    return clamp((t - h) * 16.0, 0.0, 1.0);
}

// The one impostor switch (both sides use it, so there is no gap and no overlap): an instance with an
// impostor atlas whose world sphere (without the WPO margin) projects below r_vgeo_impostor_px across
// draws its impostor quad, else its DAG cut. Never in orthographic (shadow) views. CPU mirror:
// vgeo_use_impostor.
bool vg_use_impostor(VgMesh m, vec4 s, float wpo);

// Animated culling sphere of a skinned cluster (instance space) from its bone set at pool word w
// (vgeo_gpu.h): the box around the union over its bones of AABB(palette[b] * box_b), conservative for
// linear blend skinning. CPU mirror: vgeo_skin_cluster_sphere.
vec4 vg_skin_bounds(VgXf x, uint w) {
    uint n = min(pool[w], 16u);
    vec3 mn = vec3(3.0e38), mx = vec3(-3.0e38);
    for (uint i = 0u; i < n; i++) {
        uint e0 = pool[w + 1u + i * 4u], e1 = pool[w + 2u + i * 4u], e2 = pool[w + 3u + i * 4u], e3 = pool[w + 4u + i * 4u];
        vec3 lo = vec3(unpackHalf2x16(e0).y, unpackHalf2x16(e1)), hi = vec3(unpackHalf2x16(e2), unpackHalf2x16(e3).x);
        mat4 p = palette[x.first + (e0 & 0xFFu)];
        vec3 c = (lo + hi) * 0.5, e = (hi - lo) * 0.5;
        vec3 cc = (p * vec4(c, 1.0)).xyz, ee = abs(p[0].xyz) * e.x + abs(p[1].xyz) * e.y + abs(p[2].xyz) * e.z;
        mn = min(mn, cc - ee);
        mx = max(mx, cc + ee);
    }
    vec3 c = (mn + mx) * 0.5, h = (mx - mn) * 0.5;
    float hl = length(h);
    return vec4(c, hl + 1e-5 * (length(c) + hl) + 1e-6);
}

// ------------------------------------------------------------------ tests

uint vg_flags() { return floatBitsToUint(V.params.w); }

bool vg_frustum(vec4 s) {
    if ((vg_flags() & VGV_NO_FRUSTUM) != 0u) return true;
    for (int p = 0; p < 6; p++)
        if (dot(V.planes[p].xyz, s.xyz) + V.planes[p].w < -s.w) return false;
    return true;
}

// Projected error in pixels (CPU mirror: project_error in vgeo_scene.c).
float vg_project_error(vec4 s, float err) {
    if ((vg_flags() & VGV_ORTHO) != 0u) return err * V.cam_pos.w;
    float d = max(length(s.xyz - V.cam_pos.xyz) - s.w, V.params.z);
    return err * V.cam_pos.w / d;
}

// Projected diameter in pixels (SW / HW classification).
float vg_project_size(vec4 s) {
    if ((vg_flags() & VGV_ORTHO) != 0u) return 2.0 * s.w * V.cam_pos.w;
    float d = max(length(s.xyz - V.cam_pos.xyz) - s.w, V.params.z);
    return 2.0 * s.w * V.cam_pos.w / d;
}

bool vg_use_impostor(VgMesh m, vec4 s, float wpo) {   // s: the WPO-inflated sphere (margin wpo = x.wpo); the switch measures the mesh's own size
    uint k = m.impostor_grid >> 16;   // the mesh's switch scale (1/16 units, 0 = 1x)
    float px = V.foliage.w * (k != 0u ? float(k) / 16.0 : 1.0);
    return m.impostor != 0xFFFFFFFFu && V.foliage.w > 0.0 && (vg_flags() & VGV_ORTHO) == 0u && vg_project_size(vec4(s.xyz, s.w - wpo)) < px;
}

// Per-pixel dither threshold in [0, 1) (interleaved gradient noise), rotated per frame for TAA.
float vg_dither(ivec2 px) {
    vec2 p = vec2(px) + 5.588238 * float(V.misc.x & 63u);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Hi-Z (MAX, depth 0 = near, half resolution: texture mip j covers 2^(j+1) depth pixels) occlusion of
// a sphere: its box corners projected with vp; occluded when the nearest corner depth is behind the
// farthest depth of the <= 2x2 texels covering the rect. info.xy = depth size in pixels.
bool vg_occluded_vp(vec4 s, mat4 vp, vec4 info, bool cur) {
    if ((vg_flags() & VGV_NO_OCCLUSION) != 0u || info.w < 0.5) return false;
    vec3 mn = vec3(1e30), mx = vec3(-1e30);
    for (int i = 0; i < 8; i++) {
        vec3 c = s.xyz + s.w * vec3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0, (i & 4) != 0 ? 1.0 : -1.0);
        vec4 p = vp * vec4(c, 1.0);
        if (p.w <= 1e-4) return false;   // crosses the camera plane
        vec3 ndc = p.xyz / p.w;
        mn = min(mn, ndc);
        mx = max(mx, ndc);
    }
    if (mn.z <= 0.0) return false;
    vec2 lo = (mn.xy * 0.5 + 0.5) * info.xy - 1.0, hi = (mx.xy * 0.5 + 0.5) * info.xy + 1.0;   // +1 px: TAA jitter
    lo = max(lo, vec2(0.0));
    hi = min(hi, info.xy - 1.0);
    if (any(greaterThan(lo, hi))) return false;
    float ext = max(hi.x - lo.x, hi.y - lo.y);
    int lvl = max(int(ceil(log2(max(ext, 1.0)))), 1);   // footprint 2^lvl pixels >= the rect
    if (float(lvl - 1) > info.z - 1.0) return false;   // wider than the coarsest 2x2: not conservative, keep it
    int mip = lvl - 1;
    ivec2 sz = cur ? textureSize(hiz_cur, mip) : textureSize(hiz_prev, mip);
    ivec2 a = min(ivec2(lo) >> lvl, sz - 1), b = min(ivec2(hi) >> lvl, sz - 1);
    float z;
    if (cur) z = max(max(texelFetch(hiz_cur, a, mip).r, texelFetch(hiz_cur, ivec2(b.x, a.y), mip).r), max(texelFetch(hiz_cur, ivec2(a.x, b.y), mip).r, texelFetch(hiz_cur, b, mip).r));
    else z = max(max(texelFetch(hiz_prev, a, mip).r, texelFetch(hiz_prev, ivec2(b.x, a.y), mip).r), max(texelFetch(hiz_prev, ivec2(a.x, b.y), mip).r, texelFetch(hiz_prev, b, mip).r));
    return mn.z > z;
}
#ifndef VG_NO_PUSH
// Pass 1 tests against last frame's Hi-Z (reprojected with its view); pass 2 against this frame's.
bool vg_occluded(vec4 s) {
    if (pc.pass == 1u) return vg_occluded_vp(s, V.prev_view_proj, V.hiz_prev, false);
    return vg_occluded_vp(s, V.view_proj, V.hiz_cur, true);
}
#endif

// Bounded append: returns the slot or ~0 (and counts the overflow).
uint vg_append(uint counter, uint cap) {
    uint i = atomicAdd(ctr.n[counter], 1u);
    if (i >= cap) {
        atomicAdd(ctr.n[VGN_OVERFLOW], 1u);
        return 0xFFFFFFFFu;
    }
    return i;
}

// Compute kernels define VG_COMPUTE before including (graphics stages include this file too).
#ifdef VG_COMPUTE
uint vg_flat_group() { return gl_WorkGroupID.y * 65535u + gl_WorkGroupID.x; }
#endif
