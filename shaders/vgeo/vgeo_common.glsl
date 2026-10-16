// Virtualized geometry: GLSL mirror of src/render/vgeo/vgeo_gpu.h and the shared cull helpers.
// Change a struct here and in vgeo_gpu.h together (sizes are asserted on the C side).
#extension GL_EXT_scalar_block_layout : require

struct VgViewGpu {
    mat4 view_proj;
    mat4 prev_view_proj;
    vec4 planes[6];
    vec4 cam_pos;            // xyz, w = proj_scale
    vec4 params;             // x error_px, y sw_px, z znear, w flags (uint bits)
    vec4 viewport;           // w, h, 1 / w, 1 / h
    vec4 hiz_prev;           // w, h, mips, valid
    vec4 hiz_cur;
    uvec4 counts;            // cell_count, instance_count, max_depth, -
    uvec4 caps0;             // nodes (per region), groups (per region), visible (per list), occluded
    uvec4 caps1;             // cells (per region), instances, requests, -
    mat4 raster_view_proj;   // the view the visible clusters are rasterised with (main view: jittered)
};
struct VgMaterial { vec4 color; float roughness, metallic, emissive, tex_scale; uint flags; uint albedo_tex; uint pad0, pad1, pad2, pad3, pad4, pad5; };
struct VgNode { vec4 sphere; float max_parent_error; uint first; uint count_flags; uint pad; };
struct VgGroup { vec4 lod; float parent_error; uint page; uint first_count; uint level; };
struct VgMesh { vec4 sphere; uint root_node, node_base, group_base, page_base; uint page_count, flags, material_base, impostor; float max_wpo, q_step, pad0, pad1; };
struct VgInstance { vec3 pos; uint rot; uint scale_mesh; uint flags_phase; uint extra; uint cell; };
struct VgCell { vec3 mn; uint first; vec3 mx; uint count; };

#define VGV_NO_FRUSTUM 1u
#define VGV_NO_CONE 2u
#define VGV_NO_OCCLUSION 4u
#define VGV_ORTHO 8u
#define VGV_TWO_PASS 16u
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
#define VGF_HAS_COLOR 4u
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

layout(push_constant, scalar) uniform Push { uint pass; uint in_region; uint out_region; uint view; uint a[8]; } pc;

layout(set = 0, binding = 0, scalar) readonly buffer Views { VgViewGpu views[]; };
layout(set = 0, binding = 1, scalar) readonly buffer Meshes { VgMesh meshes[]; };
layout(set = 0, binding = 2, scalar) readonly buffer Nodes { VgNode nodes[]; };
layout(set = 0, binding = 3, scalar) readonly buffer Groups { VgGroup groups[]; };
layout(set = 0, binding = 4) readonly buffer PageTable { uint page_table[]; };
layout(set = 0, binding = 5) readonly buffer Pool { uint pool[]; };
layout(set = 0, binding = 6, scalar) readonly buffer Instances { VgInstance instances[]; };
layout(set = 0, binding = 7, scalar) readonly buffer Cells { VgCell cells[]; };
layout(set = 0, binding = 8) coherent buffer Counters { uvec4 args[8]; uint n[32]; } ctr;
layout(set = 0, binding = 9) buffer CellList { uint cell_list[]; };
layout(set = 0, binding = 10) buffer InstList { uint inst_list[]; };
layout(set = 0, binding = 11) buffer NodeQueue { uvec2 node_queue[]; };
layout(set = 0, binding = 12) buffer GroupItems { uvec2 group_items[]; };
layout(set = 0, binding = 13) buffer Visible { uvec2 visible[]; };
layout(set = 0, binding = 14) buffer Occluded { uvec2 occluded[]; };
layout(set = 0, binding = 15) buffer Requests { uvec2 requests[]; };
layout(set = 0, binding = 16) uniform sampler2D hiz_prev;
layout(set = 0, binding = 17) uniform sampler2D hiz_cur;
layout(set = 0, binding = 19, scalar) readonly buffer Materials { VgMaterial materials[]; };
// Visibility buffer (binding 18, see vgeo_gpu.h): define VG_VIS before including (needs int64).
#ifdef VG_VIS
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require
layout(set = 0, binding = 18) coherent buffer VisBuffer { uint64_t vis[]; };
#endif

#define V views[pc.view]

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
struct VgXf { vec3 pos; vec4 rot; float scale; uint mesh; };
VgXf vg_instance_xf(uint i) {
    VgInstance in_ = instances[i];
    VgXf x;
    x.pos = in_.pos;
    x.rot = vg_quat_decode(in_.rot);
    x.scale = unpackHalf2x16(in_.scale_mesh & 0xFFFFu).x;
    x.mesh = in_.scale_mesh >> 16;
    return x;
}
vec4 vg_world_sphere(VgXf x, vec4 s) { return vec4(x.pos + vg_qrot(x.rot, s.xyz * x.scale), s.w * x.scale); }

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
// Pass 1 tests against last frame's Hi-Z (reprojected with its view); pass 2 against this frame's.
bool vg_occluded(vec4 s) {
    if (pc.pass == 1u) return vg_occluded_vp(s, V.prev_view_proj, V.hiz_prev, false);
    return vg_occluded_vp(s, V.view_proj, V.hiz_cur, true);
}

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
