// Physically based sky (S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique", EGSR 2020): transmittance LUT, multiple scattering LUT, sky-view LUT and
// aerial perspective froxels; src/render/sky/sky.c. Distances in km inside the atmosphere,
// radiance in engine units (the light illuminances are multiplied in), world Y up.
//
// Bindings (post universal layout): 0 transmittance, 1 multiple scattering, 2 sky view,
// 3 aerial perspective (3D), 4 environment (octahedral), 5 noise, 6 depth; 8..11 storage images;
// 12 PostUBO; 13 SkyParams; 14 SH buffer.
// sky.c skips the sky view, aerial, cloud, cloud shadow and environment row passes while the fields
// they read are unchanged (PF-0026 keys): a pass that starts reading another field needs it in its key.
#include "post_common.glsl"

layout(std430, set = 0, binding = 13) readonly buffer SkyParams {
    vec4 sun_dir;       // xyz towards the sun, w sun illuminance above the atmosphere
    vec4 moon_dir;      // xyz towards the moon, w moonlight illuminance (phase applied)
    vec4 sun_disc;      // x cos(angular radius), y angular radius, z disc radiance scale, w limb darkening u
    vec4 moon_disc;     // x cos(angular radius), y angular radius, z earthshine, w illuminated fraction
    vec4 cel_x;         // world -> celestial row X, w star brightness
    vec4 cel_y;         // row Y, w milky way brightness
    vec4 cel_z;         // row Z (north celestial pole), w night sky floor
    vec4 clouds;        // x coverage 0..1, y altitude m, zw wind offset (m)
    vec4 clouds2;       // x opacity, y time, z feature scale (m), w cloud shadow optical depth (Beer-Lambert)
    vec4 cam;           // xyz camera (world m), w camera altitude (km, >= 1 m)
    vec4 ground;        // rgb ground albedo, w ambient scale
    vec4 ap;            // x aerial perspective max distance (m), y world -> atmosphere distance scale, z mie density scale, w frame
    vec4 cshadow;       // xy origin (world xz), z size (m), w 1 / size
    vec4 env;           // x SH blend k, y env slice (0..3), z env mips, w debug view
    vec4 key;           // xyz key light direction (shadowed light), w moon is the key light
    vec4 clouds3;       // x edge softness (weather), y darkness (storm), zw unused
} SK;

const float R_BOTTOM = 6360.0;
const float R_TOP = 6460.0;
const vec3 RAY_SCAT = vec3(5.802, 13.558, 33.1) * 1e-3;
const float RAY_H = 8.0;
const float MIE_SCAT = 3.996e-3;
const float MIE_EXT = 4.40e-3;
const float MIE_H = 1.2;
const float MIE_G = 0.8;
const vec3 OZONE_ABS = vec3(0.650, 1.881, 0.085) * 1e-3;

const ivec2 TRANS_SIZE = ivec2(256, 64);
const ivec2 MS_SIZE = ivec2(32, 32);
const ivec2 SKYVIEW_SIZE = ivec2(192, 108);
const int CLOUD_MAP = 512;

// Upper hemisphere <-> square (octahedral, rotated 45 deg so the hemisphere fills it): the cloud map.
vec2 hemi_uv(vec3 d) {
    vec2 p = d.xz / (abs(d.x) + abs(d.y) + abs(d.z));
    return vec2(p.x + p.y, p.x - p.y) * 0.5 + 0.5;
}
vec3 hemi_dir(vec2 uv) {
    vec2 s = uv * 2.0 - 1.0;
    vec2 p = vec2(s.x + s.y, s.x - s.y) * 0.5;
    float y = 1.0 - abs(p.x) - abs(p.y);
    return normalize(vec3(p.x, max(y, 0.0), p.y));
}

// Nearest intersection t >= 0 of a ray from ro (planet centre at the origin) with a sphere, -1 if none.
float ray_sphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    float s = sqrt(d);
    float t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0) return t0;
    if (t1 >= 0.0) return t1;
    return -1.0;
}

struct Medium { vec3 scat_ray; float scat_mie; vec3 ext; };
Medium medium(float h) {
    Medium m;
    float dr = exp(-max(h, 0.0) / RAY_H);
    float dm = exp(-max(h, 0.0) / MIE_H) * max(SK.ap.z, 0.0);
    float doz = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    m.scat_ray = RAY_SCAT * dr;
    m.scat_mie = MIE_SCAT * dm;
    m.ext = m.scat_ray + MIE_EXT * dm + OZONE_ABS * doz;
    return m;
}

float phase_rayleigh(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }
float phase_mie(float c) {
    // Cornette-Shanks
    float g = MIE_G, g2 = g * g;
    float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

// Transmittance LUT parametrization (Bruneton): uv <-> (r, mu).
vec2 trans_params(vec2 uv) {
    float H = sqrt(R_TOP * R_TOP - R_BOTTOM * R_BOTTOM);
    float rho = H * uv.y;
    float r = sqrt(rho * rho + R_BOTTOM * R_BOTTOM);
    float d_min = R_TOP - r, d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    float mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    return vec2(r, clamp(mu, -1.0, 1.0));
}
vec2 trans_uv(float r, float mu) {
    float H = sqrt(R_TOP * R_TOP - R_BOTTOM * R_BOTTOM);
    float rho = sqrt(max(r * r - R_BOTTOM * R_BOTTOM, 0.0));
    float disc = r * r * (mu * mu - 1.0) + R_TOP * R_TOP;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float d_min = R_TOP - r, d_max = rho + H;
    return vec2((d - d_min) / max(d_max - d_min, 1e-5), rho / H);
}

// Multiple scattering LUT: (cos sun zenith, height).
vec2 ms_uv(float r, float mu_s) {
    vec2 uv = vec2(mu_s * 0.5 + 0.5, (r - R_BOTTOM) / (R_TOP - R_BOTTOM));
    return clamp(uv, vec2(0.5) / vec2(MS_SIZE), vec2(1.0) - vec2(0.5) / vec2(MS_SIZE));
}

// Sky-view LUT: absolute world azimuth (u) and a view zenith concentrated at the horizon (v).
vec2 skyview_uv(float r, vec3 dir) {
    float v_horizon = sqrt(max(r * r - R_BOTTOM * R_BOTTOM, 0.0));
    float beta = acos(clamp(v_horizon / r, -1.0, 1.0));
    float zha = PI - beta;
    float vz = acos(clamp(dir.y, -1.0, 1.0));
    float v;
    if (vz < zha) {
        float c = vz / zha;
        c = 1.0 - c;
        c = sqrt(max(c, 0.0));
        c = 1.0 - c;
        v = c * 0.5;
    } else {
        float c = (vz - zha) / max(beta, 1e-4);
        c = sqrt(max(c, 0.0));
        v = c * 0.5 + 0.5;
    }
    float az = atan(dir.x, -dir.z);   // from north (-Z), clockwise (east = +X)
    float u = az / (2.0 * PI);
    u = u < 0.0 ? u + 1.0 : u;
    return vec2(u, v);
}
vec3 skyview_dir(float r, vec2 uv) {
    float v_horizon = sqrt(max(r * r - R_BOTTOM * R_BOTTOM, 0.0));
    float beta = acos(clamp(v_horizon / r, -1.0, 1.0));
    float zha = PI - beta;
    float vz;
    if (uv.y < 0.5) {
        float c = 2.0 * uv.y;
        c = 1.0 - c;
        c *= c;
        c = 1.0 - c;
        vz = zha * c;
    } else {
        float c = uv.y * 2.0 - 1.0;
        c *= c;
        vz = zha + beta * c;
    }
    float az = uv.x * 2.0 * PI;
    float s = sin(vz);
    return vec3(s * sin(az), cos(vz), -s * cos(az));
}

// Octahedral direction encoding for the environment map.
vec2 oct_encode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.y >= 0.0 ? n.xz : (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
vec3 oct_decode_dir(vec2 uv) {
    vec2 e = uv * 2.0 - 1.0;
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// Light reaching altitude r from direction L: the transmittance LUT, zero in the planet's shadow
// (the LUT has no below-horizon rays).
float earth_vis(float r, vec3 L) { return ray_sphere(vec3(0.0, r, 0.0), L, R_BOTTOM) > 0.0 ? 0.0 : 1.0; }

// Which lights matter for the scattering: the moon only while the sun is down, the sun until
// 20 deg below the horizon (no measurable twilight beyond).
float light_weight(int k) {
    float sun_up = normalize(SK.sun_dir.xyz).y;
    if (k == 0) return (SK.sun_dir.w > 0.0 && sun_up > -0.35) ? SK.sun_dir.w : 0.0;
    return (SK.moon_dir.w > 0.0 && sun_up < 0.05) ? SK.moon_dir.w : 0.0;
}

// Camera position on the planet (km): the world origin sits on the ground.
vec3 planet_pos() { return vec3(0.0, R_BOTTOM + max(SK.cam.w, 0.001), 0.0); }

// --- 2D cloud layer (shared by the sky, the environment map and the cloud shadow map) ---
// Integer hash (stable for the large coordinates of the drifting cloud layer).
uint sk_pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float sk_hashi(ivec2 c) { return float(sk_pcg(uint(c.x) + sk_pcg(uint(c.y)))) / 4294967295.0; }
float sk_hash(vec2 p) { return sk_hashi(ivec2(floor(p))); }
float sk_vnoise(vec2 p) {
    vec2 fl = floor(p), f = p - fl;
    ivec2 i = ivec2(fl);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(sk_hashi(i), sk_hashi(i + ivec2(1, 0)), u.x), mix(sk_hashi(i + ivec2(0, 1)), sk_hashi(i + ivec2(1, 1)), u.x), u.y);
}
float sk_fbm(vec2 p, int oct) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < oct; i++) { s += a * sk_vnoise(p); p = p * 2.03 + vec2(17.1, 9.7); a *= 0.5; }
    return s;
}
// Cloud density 0..1 at a world xz point on the layer.
float cloud_density(vec2 xz, int oct) {
    float cov = clamp(SK.clouds.x, 0.0, 1.0);
    if (cov <= 0.001) return 0.0;
    vec2 p = (xz + SK.clouds.zw) / max(SK.clouds2.z, 1.0);
    float n = sk_fbm(p, oct) * 0.7 + sk_fbm(p * 2.6 + vec2(3.1, 1.7), max(oct - 2, 1)) * 0.3;
    float soft = clamp(SK.clouds3.x, 0.0, 1.0) * 0.25;
    return smoothstep(0.92 - cov * 0.75 - soft, 1.08 - cov * 0.75 + soft * 0.4, n);
}
// Where a world-space ray from the camera meets the cloud layer (flat layer, curvature ignored).
bool cloud_hit(vec3 dir, out vec2 xz, out float dist) {
    float h = SK.clouds.y - SK.cam.y;
    if (dir.y < 0.01 || h <= 0.0) { xz = vec2(0.0); dist = 0.0; return false; }
    dist = h / dir.y;
    xz = SK.cam.xz + dir.xz * dist;
    return dist < 60000.0;
}
