// The lit body's surface stage (statements, included inside main): the material colour, vertex
// colour and texture, ground cover, glTF normal map, surface layers, grid overlays. Declares flags,
// ao_b, n, vertex_ao, base, map_uv, surf_rough, surf_metal, surf_ao. Shared with the classic deferred
// prepass (classic_deferred_gbuffer.glsl), which stores what the lit pass would shade.
    uint flags = MAT_FLAGS;
    uint ao_b = MAT_AO_BYTE;
    vec3 n = normalize(v_normal);
    if (!LIT_FRONT_FACING) n = -n;
    // MAT_AO_BYTE_VERTEX: the vertex alpha is baked AO (applied to the indirect light below), not opacity.
    bool vertex_ao = (ao_b & MAT_AO_BYTE_VERTEX) != 0u;
    vec4 base = MAT_COLOR * vec4(v_color.rgb, vertex_ao ? 1.0 : v_color.a);

    if ((flags & MAT_TEXTURED) != 0u) {
        vec4 t;
        if ((flags & MAT_TRIPLANAR) != 0u) {
            float s = MAT_PARAMS.w;
            vec3 w = abs(n);
            w /= (w.x + w.y + w.z);
            t = LIT_TEX_W(v_world.zy * s, LIT_DPDX.zy * s, LIT_DPDY.zy * s) * w.x + LIT_TEX_W(v_world.xz * s, LIT_DPDX.xz * s, LIT_DPDY.xz * s) * w.y +
                LIT_TEX_W(v_world.xy * s, LIT_DPDX.xy * s, LIT_DPDY.xy * s) * w.z;
        } else {
            t = LIT_TEX(v_uv * max(MAT_PARAMS.w, 1e-3));
        }
        base *= t;
    }
#ifdef LIT_ALPHA_MASK
    if (base.a < pbr_alpha_cutoff()) discard;
    base.a = 1.0;
#endif
    vec2 map_uv = v_uv * max(MAT_PARAMS.w, 1e-3);   // the albedo's uv (glTF maps share it)
#ifdef LIT_COVER
    if ((flags & MAT_COVER) != 0u) {
        cover_base(flags, LIT_COVER_IN, LIT_COVER_PIVOT, map_uv, base.rgb);
        // the back-face flip also turned the clump dome downward: blades are lit from above on both sides
        n = normalize(vec3(n.x, abs(n.y), n.z));
    }
#endif
#ifdef PBR_MAPS
    n = pbr_normal(flags, map_uv, v_world, n);
#endif
    float surf_rough = -1.0;   // >= 0: the surface textures' roughness replaces the material's
    float surf_metal = -1.0;   // >= 0: the metal layers' metallic replaces the material's
    float surf_ao = 1.0;       // the surface layers' baked AO (x SSAO)
#ifdef LIT_SURFACES
    if (frame.surf_info.y > 0.5) {
        if ((flags & MAT_TERRAIN) != 0u && frame.surf_map.w > 0.5) {
            if (frame.surf_info.z > 0.5) terrain_shade(v_world, n, base, 0.82 + 0.36 * v_color.a, surf_rough, surf_ao);   // docs/TERRAIN_SHADING.md
            else surf_terrain(v_world, n, base, 0.82 + 0.36 * v_color.a, surf_rough, surf_ao);
        }
        else if ((flags & MAT_LAYERED) != 0u) surf_layered(v_world, v_uv, v_color, n, base, surf_rough, surf_metal, surf_ao);
    }
    if ((flags & (MAT_TERRAIN | MAT_LAYERED)) != 0u) base.a = MAT_COLOR.a;   // the vertex alpha is a layer / variation
#endif
    if ((flags & MAT_CHECKER) != 0u) {
        vec2 c = floor(v_world.xz * 0.25);
        float k = mod(c.x + c.y, 2.0);
        base.rgb *= mix(0.8, 1.0, k);
    }
    if ((flags & MAT_GRID) != 0u && frame.params.x > 0.5) base.rgb = apply_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_FINE) != 0u) base.rgb = apply_fine_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_MOA) != 0u) base.rgb = apply_moa_grid(base.rgb, v_world, n, MAT_PARAMS.w);
#ifdef LIT_BASE_HOOK
    LIT_BASE_HOOK
#endif
