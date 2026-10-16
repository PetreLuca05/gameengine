// World-position-offset snippets of vgeo foliage materials (docs/FOLIAGE_WIND.md, docs/VGEO.md "Foliage
// support"). Pure functions, included by vg_decode.glsl (vg_vertex_world: every raster path, the shadows
// and the resolve displace the same way); CPU mirror: vgeo_wind_eval (src/render/vgeo/foliage_wind.c),
// bound: vgeo_wpo_max (foliage_support.c).
//
// One model for every snippet id (grass, bush, tree: the id only names the parameter preset): GPU Gems 3
// chapter 16 (Sousa, "Vegetation Procedural Animation and Shading in Crysis"), driven by the canonical
// wind channels of the vertex colour (R edge flutter, G branch phase, B branch bend weight):
//   1. main bending: the whole plant leans along the wind by (h / H)^2, normalised back to each
//      vertex's distance to the pivot (no stretching); sway about the lean at main_freq
//   2. detail bending: B x the chapter's four smoothed triangle waves (branch phase), mostly along the
//      wind with a vertical part
//   3. edge flutter: R x the same waves (vertex phase) along the vertex normal; damps at low wind
// The displacement buffer (trample / push) goes first and takes the wind out of the pushed plant.
//
// Inputs: prm = wpo_params (trunk, branch, leaf stiffness, full wind m/s), wa / wb = wind_params
// (height, main / detail / flutter Hz | tan max bend, settle mix, response, 0); p the undisplaced world
// position, n its world normal, ip / pscale the instance pivot / scale, col the wind weights, phase the
// instance phase (0..1), wind the field at the pivot (instant x, z, settled bend x, z), t time (s),
// layers VGW_*; disp the displacement buffer's push at p (has_disp).

float wpo_stw(float x) {   // SmoothCurve(TriangleWave(x)), signed
    float tri = abs(fract(x + 0.5) * 2.0 - 1.0);
    return tri * tri * (3.0 - 2.0 * tri) * 2.0 - 1.0;
}

vec3 wpo_veg(vec4 prm, vec4 wa, vec4 wb, vec3 p, vec3 n, vec3 ip, float pscale, vec4 col, float phase, vec4 wind, float t, uint layers, vec2 disp,
             bool has_disp, inout vec3 nrm) {
    vec3 o = vec3(0.0);
    vec3 rel = p - ip;
    float H = wa.x * pscale, h = clamp(rel.y / max(H, 1e-4), 0.0, 1.0), bf = h * h;
    float k = 1.0;
    if (has_disp) {
        float l = length(disp);
        vec2 dd = l > VG_DISPLACE_MAX ? disp * (VG_DISPLACE_MAX / l) : disp;
        float dl = min(l, VG_DISPLACE_MAX);
        o += vec3(dd.x, -0.5 * dl, dd.y) * h;
        k = 1.0 - 0.85 * dl / VG_DISPLACE_MAX;
    }
    vec2 W = wind.xy + (wind.zw - wind.xy) * wb.y;
    float s = length(W);
    vec2 d = s > 1e-4 ? W / s : vec2(1.0, 0.0);
    float lin = clamp(s / prm.w, 0.0, 1.0), r = pow(lin, wb.z) * k;
    float fj = 0.85 + 0.3 * fract(phase * 13.7 + 0.13), sj = 0.85 + 0.3 * fract(phase * 7.31 + 0.57);
    float obj = ip.x * 0.173 + ip.y * 0.091 + ip.z * 0.137 + phase * 7.0;
    float br = obj + col.g * 3.0;
    float ps = 1.7 / max(pscale, 1e-3), vtx = br + (rel.x + rel.y + rel.z) * ps;
    vec2 bvec = vec2(0.0);
    float bl = 0.0;
    if ((layers & VGW_MAIN) != 0u) {
        float tm = t * wa.y * fj;
        float om = 0.6 * wpo_stw(tm * 1.31 + obj) + 0.4 * wpo_stw(tm * 0.69 + obj * 1.618 + 0.2);
        float op = wpo_stw(tm * 0.83 + obj + 0.31);
        float a = prm.x * sj * r * (0.88 + 0.12 * om), c = prm.x * sj * r * 0.08 * op;
        bvec = vec2(d.x * a - d.y * c, d.y * a + d.x * c);
        bl = length(bvec);
        if (bl > wb.x) { bvec *= wb.x / bl; bl = wb.x; }
        vec3 nr = vec3(rel.x + bvec.x * bf * H, rel.y, rel.z + bvec.y * bf * H);
        float L = length(rel), nl = length(nr);
        float sc = (L > 1e-6 && nl > 1e-6) ? L / nl : 1.0;
        o += nr * sc - rel;
    }
    vec4 w4 = vec4(0.0);
    if ((layers & (VGW_DETAIL | VGW_FLUTTER)) != 0u) {
        float a = t * wa.w * fj + vtx, b = t * wa.z * fj + br;
        w4 = vec4(wpo_stw(a * 1.427), wpo_stw(a * 0.573), wpo_stw(b * 1.320), wpo_stw(b * 0.680));
    }
    float sx = (w4.x + w4.z) * 0.5, sy = (w4.y + w4.w) * 0.5;
    if ((layers & VGW_DETAIL) != 0u) {
        float amp = clamp(col.b, 0.0, 1.0) * prm.y * sj * r * pscale;
        o += vec3(d.x * (0.4 + 0.3 * sy), 0.6 * sy, d.y * (0.4 + 0.3 * sy)) * amp;
    }
    float rf = smoothstep(0.3, 2.5, s) * (0.35 + 0.65 * lin) * k;
    if ((layers & VGW_FLUTTER) != 0u) o += n * (clamp(col.r, 0.0, 1.0) * prm.z * rf * pscale * sx);
    if ((layers & VGW_NORMALS) == 0u) return o;
    vec3 nn = nrm;
    if ((layers & VGW_MAIN) != 0u && bl > 1e-6) {
        // the bent stem's slope 2 h |b|: rotate about cross(up, bend direction) (Rodrigues)
        float th = atan(2.0 * h * bl), c = cos(th), sn = sin(th);
        vec3 ax = vec3(bvec.y / bl, 0.0, -bvec.x / bl);
        nn = nn * c + cross(ax, nn) * sn + ax * dot(ax, nn) * (1.0 - c);
    }
    if ((layers & VGW_FLUTTER) != 0u) {
        float tilt = clamp(col.r, 0.0, 1.0) * rf * min(prm.z * 10.0, 1.0) * 0.6 * w4.y;
        vec3 d3 = vec3(d.x, 0.0, d.y);
        nn += (d3 - nn * dot(d3, nn)) * tilt;
    }
    float l = length(nn);
    if (l > 1e-6) nrm = nn / l;
    return o;
}
