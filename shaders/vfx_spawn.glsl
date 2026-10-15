// Particle initialization (emit kernels). Mirrors vfx_base_dir / shape_offset in src/vfx/vfx_sim.c.

vec3 base_dir(uint dir, float bias, vec4 rot, vec3 n, vec3 incoming, inout uint s, uint counter) {
    vec3 fwd = quat_rot(rot, vec3(0, 0, -1));
    n = dot(n, n) > 1e-6 ? normalize(n) : vec3(0, 1, 0);
    vec3 inc = dot(incoming, incoming) > 1e-6 ? normalize(incoming) : vec3(0, 0, -1);
    if (dir == 1u) return n;
    if (dir == 2u) { vec3 r = reflect(inc, n); return normalize(r * (1.0 - bias) + n * bias); }
    if (dir == 3u) return inc;
    if (dir == 4u) return vec3(0, 1, 0);
    if (dir == 5u) return rand_unit(s);
    if (dir == 6u) {
        vec3 right = quat_rot(rot, vec3(1, 0, 0)), up = quat_rot(rot, vec3(0, 1, 0));
        vec3 side = (counter & 1u) != 0u ? right : -right;
        return normalize(side + up * 0.35 + fwd * 0.15);
    }
    if (dir == 7u) {
        vec3 along = inc - n * dot(inc, n);
        if (dot(along, along) < 1e-6) { vec3 t, b; basis(n, t, b); along = t; }
        return normalize(normalize(along) * (1.0 - bias) + n * bias);
    }
    return fwd;
}

vec3 shape_offset(Emitter e, vec3 axis, inout uint s) {
    uint shape = uint(e.spawn0.x);
    float radius = e.spawn0.y;
    if (shape == 1u) return rand_unit(s) * radius * pow(rand01(s), 1.0 / 3.0);
    if (shape == 2u) return vec3(rand01(s) * 2.0 - 1.0, rand01(s) * 2.0 - 1.0, rand01(s) * 2.0 - 1.0) * e.spawn1.xyz;
    if (shape == 4u || shape == 5u) {
        vec3 t, b;
        basis(axis, t, b);
        float r = radius * sqrt(rand01(s)), a = rand01(s) * 6.2831853;
        return t * (r * cos(a)) + b * (r * sin(a));
    }
    if (shape == 3u) return axis * rand01(s) * radius;
    return vec3(0);
}

// Pops a dead slot and initializes a particle. Returns false when the pool is exhausted.
bool spawn_particle(uint emitter, vec3 pos, vec4 rot, vec3 n, vec3 incoming, float scale, float energy, float alpha, vec3 vel_inherit,
                    float time_offset, uint seed, uint variant, uint flags, uint instance, uint counter, uint cur, vec3 tint) {
    int slot = int(atomicAdd(ctr.dead, uint(-1))) - 1;
    if (slot < 0) {
        atomicAdd(ctr.dead, 1u);
        atomicAdd(ctr.emitted.y, 1u);
        return false;
    }
    uint idx = dead_list[slot];
    Emitter e = emitters[emitter];
    uint s = vfx_hash(seed ^ (counter * 2654435761u));
    uint dirm = uint(e.spin_misc.w);
    vec3 axis = base_dir(dirm, e.spawn1.w, rot, n, incoming, s, counter);
    vec3 d = cone_dir(axis, e.spawn0.z, s);
    bool local = ((e.modes.x >> 16) & EF_LOCAL) != 0u && instance != 0xFFFFu;
    vec3 p = shape_offset(e, axis, s) + axis * e.spawn0.w * scale;
    float speed = range2(e.speed_life.xy, s) * energy;
    vec3 v = d * speed + vel_inherit;
    if (local) {
        // Local space: relative to the instance transform (rotation removed).
        vec4 iq = vec4(-rot.xyz, rot.w);
        p = quat_rot(iq, p);
        v = quat_rot(iq, v);
    } else {
        p += pos;
    }
    float life = max(range2(e.speed_life.zw, s), 1e-3);
    float size = range2(e.size_rot.xy, s) * scale;
    float r = range2(e.size_rot.zw, s);
    float spin = range2(e.spin_misc.xy, s);
    float cm = rand01(s);
    vec4 col = mix(e.color_a, e.color_b, cm);
    col.a *= alpha;
    col.rgb *= tint;
    col.rgb = sqrt(clamp(col.rgb, 0.0, 1.0));   // rgba8 storage with more precision in the darks (squared on read)
    uint frames = max((e.modes.z >> 16) & 0xFFFFu, 1u);
    float frame = variant != 0xFFFu ? float(variant) : ((((e.modes.x >> 16) & EF_FRAME_RANDOM) != 0u) ? floor(rand01(s) * float(frames)) : 0.0);
    // Sub-tick spawn time: particles spawned earlier in the frame start slightly older / further.
    p += v * time_offset;
    p_pos_age[idx] = vec4(p, time_offset);
    p_vel_life[idx] = vec4(v, life);
    p_misc[idx] = vec4(size, r, spin, frame);
    // Colors above 1 (HDR tints) are scaled into the emissive multiplier instead of the rgba8.
    p_info[idx] = uvec4(emitter | (flags << 16), s, pack_rgba8(col), (local ? instance : 0xFFFFu) | (uint(frame) << 16));
    uint a = atomicAdd(ctr.alive[cur], 1u);
    if (cur == 0u) alive_a[a] = idx; else alive_b[a] = idx;
    atomicAdd(ctr.emitted.x, 1u);
    return true;
}
