// Seasons (docs/MENU_LOBBY.md "Time, season and weather"): assets/world/seasons.json. A season sets
// the calendar day (sun path, day length: docs/DAYNIGHT.md), a temperature offset (precipitation
// below 0 C falls as snow: docs/WEATHER.md), which weather presets the random scheduler may pick
// and how often, and the ground's starting snow cover / wetness. The lobby picks a season, a start
// time and a start weather (one of the season's presets, or a weighted random pick). Pure.
#ifndef DG_SEASON_H
#define DG_SEASON_H

#include "core/base.h"
#include "world/weather.h"

typedef enum Season { SEASON_SPRING, SEASON_SUMMER, SEASON_AUTUMN, SEASON_WINTER, SEASON_COUNT } Season;

#define SEASON_MAX_WEATHER 16

typedef struct SeasonDef {
    char id[16];                 // "spring"
    char name[24];               // "Spring"
    int day_of_year;             // 1..365
    float temp_offset;           // C added to every weather target while the season is active
    float snow, wetness;         // starting ground snow cover / wetness (0..1)
    int weather_count;
    char weather[SEASON_MAX_WEATHER][24];   // allowed presets ...
    float weight[SEASON_MAX_WEATHER];       // ... and how likely each is (scheduler, random start)
} SeasonDef;

typedef struct SeasonTable { SeasonDef s[SEASON_COUNT]; } SeasonTable;

void seasons_defaults(SeasonTable *t);
// Parses { "seasons": [ { "id": "spring", ... } ] } over the defaults. false + err on a bad entry.
bool seasons_parse(SeasonTable *t, const char *json, char *err, int err_size);
bool seasons_load(SeasonTable *t, const char *path);   // defaults when missing
const char *season_id(int s);                           // "spring" .. "winter"
int season_from_id(const char *id);                     // -1 if unknown
bool season_allows(const SeasonDef *s, const char *preset);
// Scheduler bias: a weight per preset index of lib (0 = never in this season).
void season_weather_bias(const SeasonDef *s, const WeatherLib *lib, float *out /* WEATHER_MAX_PRESETS */);
// A weighted random start preset (deterministic for a seed). "" if the season lists none.
const char *season_pick_weather(const SeasonDef *s, u32 seed);

#endif
