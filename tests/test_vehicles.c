// Vehicle rules unit tests (src/gameplay/vehicle_def.c, docs/VEHICLES.md): definitions (defaults,
// JSON overrides, the shipped car / moto files), the fuel consumption curve and tank range, the tire
// model (hits -> leak -> burst, grip curve, rim, rolling radius, pull direction, repair / spare),
// the one-handed steering clamp and keyboard smoothing, impact damage, replication quantization.
#include "gameplay/vehicle_def.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void test_defs(void) {
    VehicleDef car, moto;
    vehicle_def_defaults(&car, VK_CAR);
    vehicle_def_defaults(&moto, VK_MOTO);
    CHECK(car.wheel_count == 4 && car.seat_count == 4 && car.seats[0].driver && !car.seats[1].driver, "car layout");
    CHECK(moto.wheel_count == 2 && moto.seat_count == 2 && moto.seats[0].driver, "moto layout");
    CHECK(car.wheels[0].driven && car.wheels[1].driven && !car.wheels[2].driven, "front wheel drive");
    CHECK(moto.wheels[1].driven && moto.wheels[0].max_steer_deg > 0, "moto: rear driven, front steered");
    CHECK(car.wheels[0].left && !car.wheels[1].left, "wheel sides");
    // Exits outside the body, seats inside it.
    for (int s = 0; s < car.seat_count; s++) {
        CHECK(fabsf(car.seats[s].exit.x) > car.box_half[0].x + 0.3f, "car exit %d clear of the body", s);
        CHECK(fabsf(car.seats[s].pos.x) < car.box_half[0].x, "car seat %d inside", s);
    }
    // JSON override keeps the rest.
    const char *j = "{\"name\":\"van\",\"kind\":\"car\",\"mass\":2000,\"fuel\":{\"capacity\":70},\"handling\":{\"one_hand_steer_limit\":0.2},"
                    "\"sockets\":{\"seat_1\":[0.4,0.8,0.1]},\"wheels\":[{\"pos\":[-0.9,0.6,-1.5]},{\"pos\":[0.9,0.6,-1.5]},{\"pos\":[-0.9,0.6,1.5]},{\"pos\":[0.9,0.6,1.5]}]}";
    VehicleDef v;
    CHECK(vehicle_def_parse(&v, j, strlen(j)), "parse");
    CHECK(!strcmp(v.name, "van") && v.mass == 2000 && v.fuel_capacity == 70 && v.one_hand_steer_limit == 0.2f, "overrides");
    CHECK(v.seats[1].pos.x == 0.4f && v.seats[0].pos.x == car.seats[0].pos.x, "socket override, others kept");
    CHECK(v.wheels[3].pos.z == 1.5f && v.wheels[0].radius == car.wheels[0].radius && v.wheels[0].driven, "wheel override keeps radius / drive");
    CHECK(!vehicle_def_parse(&v, "{bad json", 9), "bad json rejected");
    // Shipped files (when run from the build output).
    VehicleLib lib;
    vehicle_lib_load(&lib, "assets/vehicles");
    const VehicleDef *c = vehicle_lib_find(&lib, "car"), *m = vehicle_lib_find(&lib, "moto");
    CHECK(c && m && c->kind == VK_CAR && m->kind == VK_MOTO, "car.json / moto.json");
    CHECK(c && fabsf(c->mass - car.mass) < 1e-3f && m && m->seat_count == 2, "files match the defaults");
}

static void test_fuel(void) {
    VehicleDef d;
    vehicle_def_defaults(&d, VK_CAR);
    float idle = vehicle_fuel_rate(&d, 0, 0), half = vehicle_fuel_rate(&d, 0.5f, 0.5f), full = vehicle_fuel_rate(&d, 1, 1);
    CHECK(idle > 0 && idle < half && half < full, "monotonic %.5f %.5f %.5f", idle, half, full);
    CHECK(fabsf(idle * 3600.0f - d.fuel_idle_lph) < 1e-3f && fabsf(full * 3600.0f - d.fuel_full_lph) < 1e-3f, "idle / full l/h");
    bool mono = true;
    for (int i = 1; i <= 20; i++) if (vehicle_fuel_rate(&d, i / 20.0f, 0.5f) < vehicle_fuel_rate(&d, (i - 1) / 20.0f, 0.5f)) mono = false;
    CHECK(mono, "monotonic in throttle");
    CHECK(vehicle_fuel_rate(&d, 5, 5) == full && vehicle_fuel_rate(&d, -1, -1) == idle, "clamped inputs");
    // A full tank at full throttle lasts tens of minutes (gameplay range).
    float minutes = d.fuel_capacity / full / 60.0f;
    CHECK(minutes > 20 && minutes < 120, "full-throttle range %.0f min", minutes);
}

static void test_tires(void) {
    VehicleDef d;
    vehicle_def_defaults(&d, VK_CAR);
    TireState t;
    tire_init(&t);
    float lon, lat;
    tire_grip(&t, &lon, &lat);
    CHECK(lon > 0.99f && lat > 0.99f && tire_radius_scale(&t) > 0.999f && !tire_on_rim(&t), "new tire");
    bool burst = tire_hit(&t, &d);
    CHECK(!burst && t.leak > 0 && t.pressure < 1, "first hit punctures");
    float p0 = t.pressure;
    for (int i = 0; i < 600; i++) tire_tick(&t, 1.0f / 60.0f);
    CHECK(t.pressure < p0 && t.pressure > 0, "leaks over 10 s (%.2f -> %.2f)", p0, t.pressure);
    int hits = 1;
    while (!tire_hit(&t, &d) && hits < 10) hits++;
    CHECK(hits <= 3 && tire_on_rim(&t), "bursts after %d hits", hits + 1);
    tire_grip(&t, &lon, &lat);
    CHECK(lon < 0.3f && lat < 0.3f && tire_radius_scale(&t) < 0.85f, "rim: grip %.2f / %.2f radius %.2f", lon, lat, tire_radius_scale(&t));
    // Grip curve: full down to 70 %, then falls, never negative.
    TireState g;
    tire_init(&g);
    float prev = 2;
    bool ok = true;
    for (int i = 100; i >= 6; i -= 2) {
        g.pressure = i / 100.0f;
        tire_grip(&g, &lon, &lat);
        if (lon > prev + 1e-5f || lon <= 0) ok = false;
        prev = lon;
        if (i == 70) CHECK(lon > 0.99f, "full grip at 70 %%");
    }
    CHECK(ok, "grip falls monotonically with pressure");
    // Pull: a flat on the left pulls left, on the right pulls right, none when even.
    TireState ts[4];
    for (int i = 0; i < 4; i++) tire_init(&ts[i]);
    CHECK(fabsf(vehicle_tire_pull(&d, ts)) < 1e-5f, "no pull");
    ts[0].pressure = 0;
    CHECK(vehicle_tire_pull(&d, ts) < -0.1f, "left flat pulls left");
    ts[0].pressure = 1;
    ts[3].pressure = 0;
    CHECK(vehicle_tire_pull(&d, ts) > 0.1f, "right flat pulls right");
    VehicleDef m;
    vehicle_def_defaults(&m, VK_MOTO);
    CHECK(vehicle_tire_pull(&m, ts) == 0, "motorcycles do not pull");
    // Repair / spare.
    tire_repair(&t);
    CHECK(t.leak == 0 && t.pressure >= 0.85f, "repair kit");
    tire_replace(&t);
    CHECK(t.pressure == 1 && t.health == 1, "spare");
}

static void test_driving(void) {
    VehicleDef d;
    vehicle_def_defaults(&d, VK_CAR);
    CHECK(vehicle_steer_clamp(&d, 1, true) == d.one_hand_steer_limit && vehicle_steer_clamp(&d, -1, true) == -d.one_hand_steer_limit, "one-hand clamp");
    CHECK(vehicle_steer_clamp(&d, 0.9f, false) == 0.9f && vehicle_steer_clamp(&d, 0.1f, true) == 0.1f, "no clamp inside the limit");
    // Smoothing: reaches full lock at walking pace in ~0.3 s, less lock at speed, returns faster.
    float s = 0;
    int n = 0;
    while (s < 0.999f && n < 600) { s = vehicle_steer_smooth(s, 1, 0, 1.0f / 60.0f); n++; }
    CHECK(n > 10 && n < 30, "full lock in %d ticks", n);
    for (int i = 0; i < 300; i++) s = vehicle_steer_smooth(s, 1, 35, 1.0f / 60.0f);
    CHECK(s < 0.6f && s > 0.4f, "high-speed lock %.2f", s);
    int back = 0;
    while (s > 0.001f && back < 600) { s = vehicle_steer_smooth(s, 0, 35, 1.0f / 60.0f); back++; }
    CHECK(back < 10, "self-centres in %d ticks", back);
    CHECK(vehicle_impact_damage(&d, 3) == 0 && vehicle_impact_damage(&d, d.impact_damage_speed + 4) > 0, "impact threshold");
    CHECK(vehicle_impact_damage(&d, 20) > vehicle_impact_damage(&d, 12), "harder hits do more damage");
}

static void test_quant(void) {
    float worst = 0;
    for (int i = 0; i <= 1000; i++) {
        float v = i / 1000.0f;
        worst = fmaxf(worst, fabsf(veh_dq01(veh_q01(v)) - v));
    }
    CHECK(worst <= 0.5f / 255.0f + 1e-6f, "u8 0..1 error %.4f", worst);
    CHECK(veh_q01(-3) == 0 && veh_q01(7) == 255, "u8 clamped");
    CHECK(fabsf(veh_dqs(veh_qs(-0.37f)) + 0.37f) < 0.005f && veh_qs(2) == 127 && veh_qs(-2) == -127, "i8 -1..1");
}

int main(void) {
    test_defs();
    test_fuel();
    test_tires();
    test_driving();
    test_quant();
    printf("test_vehicles: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
