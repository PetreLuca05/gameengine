// Virtualized geometry: cluster / vertex / triangle decoding from the page pool (any stage), the CPU
// mirror is vgeo_decode_cluster (src/render/vgeo/vgeo_build.c). Include after vgeo_common.glsl.
//
// Page slot s starts at word s * VG_PAGE_WORDS: VgPageHeader (4 words), VgCluster[] (16 words each),
// vertex block, triangle block. Vertex (12 B, +4 with VGF_HAS_COLOR): u16x3 grid offset, oct normal
// 8 + 8, half2 uv [, RGBA8]. Triangle (4 B): u8 x3 local indices + spare.

struct VgClusterRef { uint slot, index; };
VgClusterRef vg_cluster_ref(uint packed) { return VgClusterRef(packed >> VG_CLUSTER_BITS, packed & ((1u << VG_CLUSTER_BITS) - 1u)); }

struct VgClusterData {
    vec4 sphere, lod;
    float error;
    uint cone, vert_count, tri_count, material, child_group;
    uint vert_word, tri_word;     // absolute pool word of the vertex / triangle blocks
    ivec3 base;
};

uint vg_cluster_word(uint slot, uint index) { return slot * VG_PAGE_WORDS + 4u + index * 16u; }

VgClusterData vg_cluster(uint slot, uint index) {
    uint w = vg_cluster_word(slot, index);
    VgClusterData c;
    c.sphere = uintBitsToFloat(uvec4(pool[w], pool[w + 1u], pool[w + 2u], pool[w + 3u]));
    c.lod = uintBitsToFloat(uvec4(pool[w + 4u], pool[w + 5u], pool[w + 6u], pool[w + 7u]));
    c.error = uintBitsToFloat(pool[w + 8u]);
    c.cone = pool[w + 9u];
    uint counts = pool[w + 10u], offs = pool[w + 11u];
    c.vert_count = counts & 0xFFu;
    c.tri_count = (counts >> 8) & 0xFFu;
    c.material = counts >> 16;
    c.child_group = pool[w + 12u];
    c.vert_word = slot * VG_PAGE_WORDS + (offs & 0x7FFFu);
    c.tri_word = slot * VG_PAGE_WORDS + ((offs >> 15) & 0x7FFFu);
    c.base = ivec3(int(pool[w + 13u]), int(pool[w + 14u]), int(pool[w + 15u]));
    return c;
}

uint vg_vertex_words(VgMesh m) { return (m.flags & VGF_HAS_COLOR) != 0u ? 4u : 3u; }

// Mesh-space position of vertex v (exactly the CPU decode: (base + offset) * q_step).
vec3 vg_vertex_pos(VgClusterData c, VgMesh m, uint v) {
    uint a = c.vert_word + v * vg_vertex_words(m);
    uint w0 = pool[a], w1 = pool[a + 1u];
    ivec3 g = c.base + ivec3(int(w0 & 0xFFFFu), int(w0 >> 16), int(w1 & 0xFFFFu));
    return vec3(g) * m.q_step;
}

vec3 vg_oct8(uint v) {
    vec2 f = vec2(float(v & 0xFFu), float((v >> 8) & 0xFFu)) / 255.0 * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 vg_vertex_normal(VgClusterData c, VgMesh m, uint v) { return vg_oct8(pool[c.vert_word + v * vg_vertex_words(m) + 1u] >> 16); }
vec2 vg_vertex_uv(VgClusterData c, VgMesh m, uint v) { return unpackHalf2x16(pool[c.vert_word + v * vg_vertex_words(m) + 2u]); }
vec4 vg_vertex_color(VgClusterData c, VgMesh m, uint v) {
    if ((m.flags & VGF_HAS_COLOR) == 0u) return vec4(1.0);
    return unpackUnorm4x8(pool[c.vert_word + v * 4u + 3u]);
}

// Local vertex indices of triangle t.
uvec3 vg_triangle(VgClusterData c, uint t) {
    uint w = pool[c.tri_word + t];
    return uvec3(w & 0xFFu, (w >> 8) & 0xFFu, (w >> 16) & 0xFFu);
}

// Instance transform of a mesh-space point / direction (uniform scale).
vec3 vg_xf_point(VgXf x, vec3 p) { return x.pos + vg_qrot(x.rot, p * x.scale); }
vec3 vg_xf_dir(VgXf x, vec3 d) { return vg_qrot(x.rot, d); }

// Visible list entry -> (instance, page slot << 12 | cluster) and the visibility buffer packing.
uvec2 vg_visible_entry(uint visible_index) { return visible[visible_index]; }
uint vg_vis_payload(uint visible_index, uint tri) { return visible_index << VG_VIS_TRI_BITS | tri; }
