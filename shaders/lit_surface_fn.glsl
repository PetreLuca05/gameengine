// Surface helpers of the lit body (grid overlays), shared with the classic deferred prepass
// (classic_deferred_gbuffer.glsl): included at global scope.
// Anti-aliased grid line intensity for coordinate c with spacing s and width w (in pixels).
float grid_line(float c, float s, float w) {
    float x = c / s;
    float d = abs(fract(x - 0.5) - 0.5) / max(fwidth(x), 1e-5);
    return 1.0 - clamp(d / w, 0.0, 1.0);
}

vec3 apply_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    vec2 axis_tint_sel;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.25, 0.8), grid_line(c.y, 0.25, 0.8));
    float major = max(grid_line(c.x, 1.0, 1.3), grid_line(c.y, 1.0, 1.3));
    float big   = max(grid_line(c.x, 5.0, 2.0), grid_line(c.y, 5.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.72, minor * 0.6);
    col = mix(col, col * 0.35, major);
    col = mix(col, col * 0.18, big * 0.6);
    // The world origin axes on floors (greys only): X near black, Z near white.
    if (an.y > 0.9) {
        float ax = grid_line(p.z, 1000.0, 2.5) * step(abs(p.z), 0.5);
        float az = grid_line(p.x, 1000.0, 2.5) * step(abs(p.x), 0.5);
        col = mix(col, vec3(0.04), ax);
        col = mix(col, vec3(0.9), az);
    }
    return col;
}

// Measuring grid for the recoil wall: 10 cm minor, 50 cm major, 1 m bold lines.
vec3 apply_fine_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.1, 0.9), grid_line(c.y, 0.1, 0.9));
    float half_m = max(grid_line(c.x, 0.5, 1.4), grid_line(c.y, 0.5, 1.4));
    float meter = max(grid_line(c.x, 1.0, 2.0), grid_line(c.y, 1.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.62, minor * 0.7);
    col = mix(col, vec3(0.32), half_m * 0.8);
    col = mix(col, vec3(0.06), meter);
    return col;
}

// Sighting-in target: 1 MOA squares at its distance (spacing s in metres), 5 MOA bold, 10 MOA black
// (level.c snaps the target centre onto a 10 MOA crossing; the aim square is its own box).
vec3 apply_moa_grid(vec3 base, vec3 p, vec3 n, float s) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    s = max(s, 1e-4);
    // Each level fades out once its squares get smaller than a few pixels (readable through a
    // scope, a clean board from far away).
    float px = max(length(fwidth(c)), 1e-6);
    float f1 = clamp((s / px - 4.0) / 4.0, 0.0, 1.0), f5 = clamp((s * 5.0 / px - 4.0) / 4.0, 0.0, 1.0), f10 = clamp((s * 10.0 / px - 4.0) / 4.0, 0.0, 1.0);
    float minor = max(grid_line(c.x, s, 0.8), grid_line(c.y, s, 0.8)) * f1;
    float five = max(grid_line(c.x, s * 5.0, 1.2), grid_line(c.y, s * 5.0, 1.2)) * f5;
    float ten = max(grid_line(c.x, s * 10.0, 1.6), grid_line(c.y, s * 10.0, 1.6)) * f10;
    vec3 col = base;
    col = mix(col, col * 0.55, minor * 0.8);
    col = mix(col, vec3(0.25), five * 0.85);
    col = mix(col, vec3(0.03), ten);
    return col;
}
