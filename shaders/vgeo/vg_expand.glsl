// Shared by the cell, instance and traversal kernels: instance test and node expansion.

// Expands a visible node: leaves emit the clusters of their groups whose parent error projects above
// the threshold as cluster candidates (instance, group << 4 | cluster within the group; pass 1 region
// 0, pass 2 region 1; a page request when the group is not resident), inner nodes push their children
// into node queue region out_region.
void vg_expand(uint inst, VgXf x, VgMesh m, VgNode nd, uint out_region) {
    float t = V.params.x;
    uint count = nd.count_flags & 0xFFFFu;
    if ((nd.count_flags >> 16 & VGN_LEAF) != 0u) {
        uint greg = pc.pass == 1u ? 0u : 1u, gcap = V.caps0.y;
        for (uint g = 0u; g < count; g++) {
            uint gi = m.group_base + nd.first + g;
            VgGroup gr = groups[gi];
            float px = vg_project_error(vg_world_sphere(x, gr.lod), gr.parent_error * x.scale);
            if (!(px > t)) continue;
            uint slot = page_table[m.page_base + gr.page];
            if (slot == 0xFFFFFFFFu) {
                uint k = vg_append(VGN_PAGE_REQ, V.caps1.z);
                if (k != 0xFFFFFFFFu) requests[k] = uvec2(m.page_base + gr.page, floatBitsToUint(min(px, 1e30)));
                continue;
            }
            uint n = gr.first_count >> 16;
            uint k0 = atomicAdd(ctr.n[VGN_GROUPS0 + greg], n);
            for (uint c = 0u; c < n; c++) {
                if (k0 + c >= gcap) { atomicAdd(ctr.n[VGN_OVERFLOW], 1u); break; }
                group_items[greg * gcap + k0 + c] = uvec2(inst, gi << 4 | c);
            }
        }
    } else {
        uint cap = V.caps0.x;
        uint k0 = atomicAdd(ctr.n[VGN_NODEQ0 + out_region], count);
        for (uint c = 0u; c < count; c++) {
            if (k0 + c >= cap) { atomicAdd(ctr.n[VGN_OVERFLOW], 1u); break; }
            node_queue[out_region * cap + k0 + c] = uvec2(inst, m.node_base + nd.first + c);
        }
    }
}

// Instance: frustum + Hi-Z on the mesh sphere (= the BVH root's), then the root's error test and
// expansion (the traversal starts at the root's children). Pass 1 occluded instances are listed for
// pass 2; out_region: node queue region of the children.
void vg_instance(uint inst, uint out_region) {
    VgXf x = vg_instance_xf(inst);
    if (!x.live) return;   // skinned instance of an empty palette slot
    VgMesh m = meshes[x.mesh];
    vec4 s = vg_world_sphere(x, m.sphere);
    if (!vg_frustum(s)) return;
    if (vg_occluded(s)) {
        if (pc.pass == 1u) {
            uint k = vg_append(VGN_INST_OCC, V.caps1.y);
            if (k != 0xFFFFFFFFu) inst_list[k] = inst;
        }
        return;
    }
    atomicAdd(ctr.n[pc.pass == 1u ? VGN_INST_VIS : VGN_INST_P2], 1u);
    VgNode root = nodes[m.node_base + m.root_node];
    if (!(vg_project_error(vg_world_sphere(x, root.sphere), root.max_parent_error * x.scale) > V.params.x)) return;
    atomicAdd(ctr.n[VGN_NODES_VISITED], 1u);
    vg_expand(inst, x, m, root, out_region);
}
