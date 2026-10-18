// Functions shared between the game/*.c files (not part of the public game API).
#ifndef DG_GAME_INTERNAL_H
#define DG_GAME_INTERNAL_H

#include "game.h"

void game_sim_player(World *w, int slot, const UserCmd *cmd, float dt, bool first);
void game_cl_reconcile(const PlayerSim *server, u32 ack_seq);
int game_spawn_player(World *w, int slot, const char *name, bool local, int peer);
void game_remove_player(World *w, int slot);
void game_respawn_player(World *w, int slot);
void game_respawn_player_at(World *w, int slot, vec3 pos, float yaw, float hp);
void game_create_player_character(World *w, int slot, vec3 feet);

// game_events.c
void game_add_hint(const char *text);
void game_hud_tick(float dt);
void game_hud_reset(void);   // new map / session: kill stats, feed, pings, wheels

// game_pings.c: pings (docs/UI_WHEELS_HUD.md "Pings")
void game_pings_register(void);
bool game_ping_target(vec3 *pos, int *kind);    // the crosshair's point + contextual kind
bool game_ping_request(int type);               // type < 0: contextual
void game_sv_ping(World *w, int slot, int a, int b, int c);
void game_sv_auto_ping(World *w, int slot, int type, vec3 pos);   // host: a ping on a player's behalf (downed / dead, docs/REVIVE.md)
void game_pings_event(const GameEvent *ev);
void game_pings_tick(float dt);
void game_pings_reset(void);

// game_ledge.c: ledge grab / climb, game side (docs/CONTROLLER.md)
void ledge_register(void);
// The hang prompts of the local player ("[Space] Climb", "[Ctrl/S] Let go"); false = none (not hanging,
// or spectating someone: the prompt is local UI).
bool ledge_hang_prompts(char *climb, size_t climb_size, char *let_go, size_t let_go_size);
void ledge_prompts_drawn(bool drawn);   // the HUD's report of the frame (tests: ledge_report)

// game_peek.c: door peek through the visor
struct PostSettings;
void peek_register(void);
bool peek_active(void);
float peek_amount(void);
const char *peek_block_reason(EntityId door);
bool peek_start(EntityId door);
void peek_stop(void);
bool peek_frame(float dt, vec2 look, bool ui_blocking);
void peek_on_hurt(void);
bool peek_camera(World *w, RenderCamera *cam);
void peek_post(struct PostSettings *s);
void peek_draw(void);

// game_lean.c: leaning out of an open window (client camera)
void lean_register(void);
bool lean_active(void);
float lean_amount(void);
bool lean_start(EntityId spot);
void lean_stop(void);
void lean_on_hurt(void);
void lean_frame(float dt, bool ui_blocking);
bool lean_camera(World *w, RenderCamera *cam);
void lean_draw(void);

// game_wheels.c: radial wheels (healing, throwables, ping, door), the HUD timed ring
void wheels_register(void);
bool wheels_frame(float dt, bool ui_blocking);  // before the look: true = the look is the wheel's
void wheels_filter_cmd(UserCmd *c);             // build_cmd: E tap / hold on doors, auto-use
float wheels_timescale(void);                   // simulation time scale while a wheel is open
bool wheels_open(void);
void wheels_on_hurt(void);
void wheels_reset(void);

// game_render.c
void game_render_init(void);
void game_render_world(World *w, float alpha, vec3 cam_pos, vec3 cam_fwd);
MeshId game_capsule_mesh(float radius, float half_height);

// game_holdpoint.c
void game_holdpoint_register(void);
void game_holdpoint_test_frame(void);
bool game_holdpoint_test_active(void);
bool holdpoint_save_all(const char *path_override);

// game_sights.c: ADS sights (FOV, sensitivity, lens / reticle / PiP hand-off, tests)
void sights_register(void);
void sights_camera(const Player *p, const WeaponDef *wd, CameraAds *out);
void sights_frame(const RenderCamera *cam, bool first_person);
void sights_test_frame(const RenderCamera *cam);
bool sights_test_running(void);
bool sights_dof_managed(void);
void sights_draw(void);

// game_daynight.c: day / night cycle
struct PostSettings;
void daynight_register(void);
void weather_register(void);                     // game_weather.c
// Foliage (game_foliage.c, docs/VGEO.md "Foliage support")
void foliage_register(void);
// game_wind.c: the wind field (docs/FOLIAGE_WIND.md)
struct WindField;
void wind_game_register(void);
void wind_game_frame(const WeatherParams *cur, double tick_time, vec3 cam, float dt, RenderWeather *rw);   // cur NULL: no weather
vec3 wind_game_at(vec3 pos);                        // instant wind (m/s): gusts, turbulence, local sources
void wind_game_blast(vec3 pos, float radius, float strength);   // an explosion's wind ring
void wind_game_wake(vec3 pos, vec3 vel, float radius);         // a vehicle's wake (call every few frames while moving)
float wind_game_leaf_rate(vec3 pos);                // leaves torn off per second per tree (leaf-shedding effects)
float wind_game_rustle(vec3 pos);                   // foliage rustle 0..1: local wind x foliage nearby (ambience)
const struct WindField *wind_game_field(void);
void wind_game_draw_debug(vec3 cam);
void wind_game_draw_panel(void);
bool wind_game_panel_open(void);
void foliage_frame(World *w);                     // players flatten grass
void foliage_clear(void);                         // the test scatter placements + their capsules
void foliage_level_reset(void);                   // map load: the placements (the old world took the capsules)
// Vehicles (game_vehicles.c, docs/VEHICLES.md)
struct Writer;
struct Reader;
void vehicles_register(void);
void vehicles_level_loaded(World *w, const char *map);
void vehicles_player_cmd(World *w, int slot, const UserCmd *cmd, u16 pressed, bool first);
void vehicles_host_tick(World *w);                // before the physics step
void vehicles_host_post(World *w);                // after it
void vehicles_test_tick(World *w);
bool vehicles_client_tick(World *w, const UserCmd *cmd);   // true: the client steps physics (prediction)
void vehicles_client_post(World *w, u32 seq);
void vehicles_net_write(World *w, struct Writer *wr);
void vehicles_net_read(World *w, struct Reader *rd, bool apply, u32 ack_seq);
void vehicles_handle_event(const GameEvent *ev);
void vehicles_input(void);
void vehicles_fill_cmd(UserCmd *c);
void vehicles_view_frame(ViewState *view);
int vehicles_camera(World *w, RenderCamera *cam);
void vehicles_frame(World *w, vec3 cam_pos);
void vehicles_draw_hud(void);
void vehicles_draw(void);
EntityId vehicle_spawn(World *w, const char *def, vec3 pos, float yaw);
// Hand pose editor (simulated vehicle cases, docs/HANDPOSE.md): seat / unseat the player, the
// steering base frame (the driver case's anchor is relative to it) and the passenger lap frame.
bool vehicles_editor_seat(World *w, EntityId veh, int slot, int seat);
void vehicles_editor_unseat(World *w, int slot);
bool vehicles_steer_base(World *w, int slot, Xform *base);
bool vehicles_seat_frame(World *w, int slot, Xform *out);
void weather_level_loaded(World *w, const char *map);
// Seasons (src/world/season.h, docs/MENU_LOBBY.md): host. season -1 = off. start "" = a weighted
// pick of the season's presets; dynamic = the season-biased scheduler changes the weather over time.
struct SeasonTable;
void weather_apply_season(World *w, int season, const char *start, bool dynamic);
const struct SeasonTable *weather_seasons(void);
int weather_season(void);
void weather_host_tick(World *w);
void weather_freeze(World *w, bool on, bool neutral);   // frozen world: hold the weather (neutral: clear sky)
// Frozen world (scene handpose, game_freeze.c): time, weather, wind, VFX time, zombies / NPCs,
// vehicle physics, props and ambience stop. Neutral lighting: 12:30 and a clear sky.
void game_world_freeze(World *w, bool on, bool neutral_light);
void game_world_freeze_lighting(World *w, bool neutral);
bool game_world_frozen(void);
bool weather_active(World *w);
bool weather_current(World *w, WeatherParams *out);
void weather_frame(World *w, const RenderCamera *cam, const Player *lp);   // before daynight_environment
void weather_environment(World *w, RenderEnvironment *env);               // sky hooks (daynight)
void weather_post(struct PostSettings *s);
float weather_cloud_cover(World *w, float fallback);
float weather_fog_density(World *w, float fallback);
float weather_lightning_flash(void);
void weather_draw(void);
// Audio glue (game_audio.c, docs/AUDIO_PROCEDURAL.md): ray / foliage queries for the environment
// chain, the listener state each frame (before audio_update), the audio_debug overlay.
void game_audio_register(void);
void game_audio_frame(World *w, const RenderCamera *cam, const Player *p, bool ads);
void game_audio_draw(vec3 cam_pos);
void daynight_level_loaded(World *w);
void daynight_story_drive(World *w, float hours, float minutes_per_second);   // story mode: the beat clock drives the time
bool weather_set_preset(World *w, const char *spec, float seconds);           // host: preset / composition over seconds (story mode)
// Host: calendar day (1..365, the season's sun path / day length) and the time (hours < 0 = keep).
void daynight_set_calendar(World *w, int day_of_year, float hours);
void daynight_host_tick(World *w);
void daynight_handle_event(const GameEvent *ev);
bool daynight_active(World *w);
void daynight_environment(World *w, const RenderCamera *cam, RenderEnvironment *env);
void daynight_post(struct PostSettings *s);
bool daynight_preset_time(World *w, float elev_deg, bool moon_full);   // shadow presets: evening sun elevation / full moon midnight
bool daynight_preset_morning(World *w, float elev_deg);                 // shadow presets: the morning sun first above elev_deg
// game_terrain.c: terrain shading level / debug / reload / paint tool (docs/TERRAIN_SHADING.md)
void terrain_game_register(void);
// game_ground_ref.c: reference viewer, camera presets, colour sampler (docs/GROUND_REFERENCE.md)
void ground_ref_register(void);
void ground_ref_draw(void);
void terrain_game_frame(vec3 cam);
// game_shadow_sky.c: sky-driven shadows (docs/SHADOWS.md)
struct ShadowSkyInput;
struct ShadowSkyOut;
void shadow_sky_register(void);
void shadow_sky_frame(struct ShadowSkyInput *in, struct ShadowSkyOut *out, float tod_cloud_wind);   // fidelity inputs + one driver step
float shadow_sky_exposure(void);                 // EV compensation of the current conditions
void shadow_sky_level_loaded(void);
void shadow_sky_draw(void);
void daynight_draw(void);
bool daynight_editor_open(void);
// game_flesh_range.c: flesh impact range panel / tests (dev room)
void flesh_range_register(void);
void flesh_range_draw(void);
bool flesh_range_open(void);

// game_post.c: post-processing settings (presets, post volumes, gameplay feedback, photo mode)
typedef struct PostFrameInfo {
    vec3 cam_pos;
    bool ads, sprinting;
    float level_fog;
} PostFrameInfo;
void game_post_init(void);
void game_settings_audit_init(void);   // settings_audit, sv_rule, consistency_check (docs/SETTINGS_AUDIT.md)
// game_look.c: the post-processing Look (docs/LOOKS.md): r_look, its finish / modulators in the frame's
// PostSettings, the Settings A/B, dev look_tune / look_dump.
struct PostSettings;
void game_look_init(void);
void game_look_frame(void);                              // per frame, before the settings are built
void game_look_apply(struct PostSettings *s);            // the look + its finish (after the cvars, before post volumes)
void game_look_modulate(struct PostSettings *s);         // time of day / weather (after daynight / weather)
int game_look_selected(void);                            // r_look
int game_look_current(void);                             // what renders (Realism while the A/B is held)
void game_look_set(int id);
void game_look_compare_hold(bool held);                  // Settings: hold to compare with Realism (per UI frame)
bool game_look_comparing(void);
// Fire animation (src/game/game_fire.c, docs/FIRE_ANIM.md): the viewed player's gun-driven fire motion.
struct FireAnimOut;
struct ShotEvent;
void game_fire_init(void);
void game_fire_on_shot(const struct ShotEvent *ev);
void game_fire_tick(void);                               // per sim tick, before chars_present_tick
const struct FireAnimOut *game_fire_sample(float alpha); // this frame's interpolated output (camera layer, hands)
void game_fire_post(float *blur, float *chroma);         // post feedback this frame
void game_fire_reset(void);
void game_fire_frame(void);                              // per frame (fire_compare driver)
void game_fire_flush_flash(void);                        // after the first-person item is placed: the muzzle flash on it
// Recorder (tuner graphs): one entry per tick, oldest first.
typedef struct FireRecord { float gun_pitch, gun_back, hand_pitch, cam_pitch, fov; bool shot, held; } FireRecord;
int game_fire_record(FireRecord *out, int max);
const struct FireAnimDef *game_fire_def(void);
// Tuner loops (single / burst / mag dump, repeated) at a simulation time scale; save a class file.
typedef enum FireLoop { FIRE_LOOP_OFF, FIRE_LOOP_SINGLE, FIRE_LOOP_BURST, FIRE_LOOP_MAGDUMP } FireLoop;
void game_fire_loop(int mode, float timescale);
float game_fire_timescale(void);
bool game_fire_loop_trigger(void);   // the loop holds the trigger this tick
bool game_fire_save_class(const struct FireAnimDef *d);
struct FireAnimState *game_fire_state(void);
void game_post_frame(const PostFrameInfo *fi);
void game_post_draw_overlay(void);
void game_post_draw_photo_ui(void);
void game_post_flash(float strength);
bool game_post_photo_mode(void);
void game_post_set_photo_mode(bool on);
int game_post_detect_preset(void);
const char *game_post_preset_name(int p);
void game_post_apply_preset(int p);
float game_post_bench_post_ms(void);
bool game_post_preset_allowed(int p, int *req_level);   // Engine Fidelity bounds allow every value of the preset

// game_fidelity.c (docs/ENGINE_FIDELITY.md)
void game_fidelity_init(void);
void game_fidelity_frame(void);
void game_fidelity_draw(void);
int game_fidelity_level(void);
void game_fidelity_set(int level);
int game_fidelity_recommended(void);
float game_fidelity_bench_ms(int level);
bool game_fidelity_busy(void);
void game_fidelity_detect(bool popup);
void game_fidelity_apply_recommended(void);

#endif
