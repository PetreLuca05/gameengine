// Chart primitives on the UI draw list (docs/STATS.md "Charts"): line, sparkline, bar, stacked area,
// donut, radar, histogram, scatter, calendar heatmap, gauge, timeline. No external library: every
// chart builds its triangles (UiVertex, the atlas' white texel) and its labels into a ChartCache
// when the data revision or the rect changes, and draws the cached buffers every frame (gd_tris +
// gd_text). Hover is answered from the cached geometry: each chart returns the data index under the
// mouse (-1 none) for the caller's tooltip (chart_tooltip).
//
// Every chart takes a ChartStyle (colours from the UI theme, sizes, padding) and a rect; empty data
// draws a placeholder message. Axes auto-scale to "nice" ticks (1 / 2 / 5 x 10^n, chart_nice_ticks).
// The math is pure (ui_chart_math.c, tests/test_stats.c).
#ifndef DG_UI_CHART_H
#define DG_UI_CHART_H

#include "core/base.h"
#include "ui/gui/gui_layout.h"

#define CHART_MAX_SERIES 12
#define CHART_MAX_TICKS 12

// ---------------------------------------------------------------- pure math (ui_chart_math.c)

typedef struct ChartTicks {
    double lo, hi, step;     // the axis range (multiples of step) and the tick step
    int count;               // ticks lo, lo + step, ... hi
} ChartTicks;
// "Nice" axis: step from 1 / 2 / 5 x 10^n so that about max_ticks ticks cover [lo, hi]; the range
// widens to multiples of the step. lo == hi gets a unit around it; include_zero pulls 0 in.
ChartTicks chart_nice_ticks(double lo, double hi, int max_ticks, bool include_zero);
double chart_nice_step(double range, int max_ticks);
// Trailing moving average (window w; the first w - 1 points average what exists). out may be in.
void chart_moving_average(const float *in, int n, int window, float *out);
// Index of the sample nearest to x among n evenly spaced over [x0, x1] (-1 outside / n == 0).
int chart_nearest_index(float x, float x0, float x1, int n);
// Nearest point (squared distance <= max_d2) among n; -1 none.
int chart_nearest_point(const float *px, const float *py, int n, float x, float y, float max_d2);
// Donut: the segment under angle a (radians, clockwise from the top, 0..2pi) for the values; -1 none.
int chart_donut_segment(const float *values, int n, float a);
// Bucket of v in [lo, hi) cut into n equal buckets (clamped).
int chart_bucket(float v, float lo, float hi, int n);
// Calendar heatmap: the column / row of day index i (0 = the oldest of `days`) when the last day is
// on weekday last_wd (0 Monday): columns are weeks, rows Monday..Sunday. Returns the column count.
int chart_calendar_cell(int i, int days, int last_wd, int *col, int *row);
// Number formatting: 1,234 / 12.3 % / 1.2 km (imperial: mi / ft) / 1h 23m.
const char *chart_fmt_int(char *buf, int size, double v);          // thousands separators
const char *chart_fmt_num(char *buf, int size, double v, int decimals);
const char *chart_fmt_pct(char *buf, int size, double frac01, int decimals);
const char *chart_fmt_dist(char *buf, int size, double meters, bool imperial);
const char *chart_fmt_duration(char *buf, int size, double seconds);
const char *chart_fmt_compact(char *buf, int size, double v);      // axis labels: 950, 1.2k, 3.4M

// ---------------------------------------------------------------- style + cache

typedef struct ChartStyle {
    u32 text, text_dim, grid, axis, panel, accent, good, bad, warn, marker, empty;
    u32 series[CHART_MAX_SERIES];
    float font, font_small, pad, line_w, point_r;
} ChartStyle;
ChartStyle chart_style_default(void);   // the current UI theme (gui_theme), scaled (gui_s)

typedef struct ChartLabel { float x, y; float size; u32 color; u8 align, weight; char text[40]; } ChartLabel;

typedef struct ChartCache {
    GuiRect rect;             // built for
    u32 rev;                  // data revision built for (caller's)
    bool built;
    struct UiVertex *v;
    u32 *idx;
    int nv, ni, cap_v, cap_i;
    ChartLabel *labels;
    int nl, cap_l;
    // hover geometry
    GuiRect plot;
    int n;                    // data points / bars / segments
    float *hx, *hy;           // point positions (line / scatter / bars / cells)
    int cap_h;
    float aux[8];             // per-chart (donut centre / radii, gauge...)
} ChartCache;
void chart_cache_free(ChartCache *c);
void chart_cache_invalidate(ChartCache *c);

// The tooltip box at the mouse (top layer), lines separated by '\n'.
void chart_tooltip(const char *text);
// Legend: coloured swatches + names in a row inside r.
void chart_legend(GuiRect r, const ChartStyle *st, const char *const *names, const u32 *colors, int n);

// ---------------------------------------------------------------- charts
// rev: the caller's data revision (rebuild when it changes). Returns the hovered index or -1.

typedef const char *(*ChartLabelFn)(int i, char *buf, int size, void *user);
typedef const char *(*ChartValueFn)(double v, char *buf, int size, void *user);

typedef struct ChartSeries {
    const float *y;           // n values (NAN: gap)
    const char *name;
    u32 color;                // 0: style series colour
    bool right_axis;          // secondary (right) Y axis
    bool fill;                // area under the line
    int ma_window;            // > 1: moving-average overlay
    bool dashed;
} ChartSeries;

typedef struct ChartLineSpec {
    int n, nseries;
    const ChartSeries *s;
    ChartLabelFn x_label;     // NULL: 1..n
    ChartValueFn y_fmt, y2_fmt;
    void *user;
    const u8 *point_class;    // NULL or n: 0 plain dot, 1 good (win), 2 bad (loss), 3 neutral (abandon); dots on series 0
    bool include_zero;
    const char *empty;        // placeholder text
} ChartLineSpec;
int chart_line(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartLineSpec *spec);
int chart_sparkline(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const float *y, int n, u32 color);

typedef struct ChartBarSpec {
    int n, groups;            // bars, series per bar (grouped)
    const float *v;           // n * groups (v[i * groups + g])
    const u32 *bar_colors;    // NULL or n: per-bar override (single series)
    const u32 *group_colors;  // NULL or groups
    bool horizontal;
    float ref;                // reference line value (NAN none)
    const char *ref_label;
    ChartLabelFn label;       // category labels
    ChartValueFn fmt;         // value labels (NULL: compact)
    void *user;
    bool value_labels;        // the value at the end of each bar
    int highlight;            // bar drawn brighter (-1 none)
    const char *empty;
} ChartBarSpec;
int chart_bar(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartBarSpec *spec);

typedef struct ChartStackSpec {
    int n, nseries;
    const float *const *y;    // nseries arrays of n
    const u32 *colors;        // NULL: style series
    ChartLabelFn x_label;
    void *user;
    const char *empty;
} ChartStackSpec;
// Returns the hovered x index; *series_out (may be NULL) the band under the mouse.
int chart_stacked_area(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartStackSpec *spec, int *series_out);

typedef struct ChartDonutSeg { float value; u32 color; const char *label; } ChartDonutSeg;
int chart_donut(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartDonutSeg *seg, int n, const char *center, const char *center_sub, bool legend);

typedef struct ChartRadarSpec {
    int axes;                 // 3..8
    const char *const *labels;
    int polys;                // 1..2
    const float *values[2];   // axes values each, 0..1
    u32 colors[2];
    const char *empty;
} ChartRadarSpec;
int chart_radar(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartRadarSpec *spec);   // hovered axis

typedef struct ChartHistSpec {
    int n;
    const float *counts;
    ChartLabelFn label;       // bucket labels
    void *user;
    float marker;             // bucket position (fractional bucket index) of the highlighted value, < 0 none
    const char *marker_label;
    u32 color;
    const char *empty;
} ChartHistSpec;
int chart_histogram(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartHistSpec *spec);

typedef struct ChartScatterSpec {
    int n;
    const float *x, *y;
    const u32 *colors;        // NULL: accent
    const float *sizes;       // NULL: style point radius (multiplier)
    const char *x_title, *y_title;
    ChartValueFn x_fmt, y_fmt;
    void *user;
    const char *empty;
} ChartScatterSpec;
int chart_scatter(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartScatterSpec *spec);

typedef struct ChartCalendarSpec {
    int days;                 // values for the last `days` days (index 0 oldest)
    const float *values;
    int last_weekday;         // weekday of the last day (0 Monday)
    ChartLabelFn month_label; // the label of a column start (day index), NULL none
    void *user;
    u32 color;                // full intensity
} ChartCalendarSpec;
int chart_calendar_heatmap(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartCalendarSpec *spec);   // hovered day index

void chart_gauge(GuiRect r, const ChartStyle *st, float value01, const char *label, const char *value_text, u32 color);

typedef struct ChartTimelineSpec {
    int n;                    // waves / segments
    const float *bars;        // kills per wave
    const u8 *downs, *revives;   // markers per wave (NULL none)
    ChartLabelFn label;
    void *user;
    const char *empty;
} ChartTimelineSpec;
int chart_timeline(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartTimelineSpec *spec);

#endif
