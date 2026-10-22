// Chart math (ui_chart.h "pure math"): nice ticks, moving average, hover lookups, calendar cells,
// number formatting. Pure (tests/test_stats.c).
#include "ui_chart.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

double chart_nice_step(double range, int max_ticks) {
    if (!(range > 0) || !isfinite(range)) return 1.0;
    if (max_ticks < 2) max_ticks = 2;
    double raw = range / (double)(max_ticks - 1);
    double mag = pow(10.0, floor(log10(raw)));
    double f = raw / mag;
    double nice = f <= 1.0 + 1e-9 ? 1 : f <= 2.0 + 1e-9 ? 2 : f <= 5.0 + 1e-9 ? 5 : 10;
    return nice * mag;
}

// The next nice step above s (1 -> 2 -> 5 -> 10).
static double next_step(double s) {
    double mag = pow(10.0, floor(log10(s) + 1e-9));
    double f = s / mag;
    return (f < 1.5 ? 2 : f < 3.5 ? 5 : 10) * mag;
}

ChartTicks chart_nice_ticks(double lo, double hi, int max_ticks, bool include_zero) {
    ChartTicks t;
    if (!isfinite(lo) || !isfinite(hi)) { lo = 0; hi = 1; }
    if (lo > hi) { double x = lo; lo = hi; hi = x; }
    if (include_zero) { lo = fmin(lo, 0.0); hi = fmax(hi, 0.0); }
    if (hi - lo < 1e-12) {
        // A flat series: a unit above zero (with zero included), else half its magnitude around it.
        double d = fabs(lo) > 1e-12 ? fabs(lo) * 0.5 : 1.0;
        if (include_zero) hi = lo + d;
        else { lo -= d; hi += d; }
    }
    if (max_ticks < 2) max_ticks = 2;
    if (max_ticks > CHART_MAX_TICKS) max_ticks = CHART_MAX_TICKS;
    double step = chart_nice_step(hi - lo, max_ticks);
    for (int guard = 0; guard < 8; guard++) {
        double a = floor(lo / step + 1e-9) * step, b = ceil(hi / step - 1e-9) * step;
        int count = (int)llround((b - a) / step) + 1;
        if (count <= max_ticks || guard == 7) {
            t.lo = fabs(a) < step * 1e-9 ? 0.0 : a;
            t.hi = fabs(b) < step * 1e-9 ? 0.0 : b;
            t.step = step;
            t.count = count;
            return t;
        }
        step = next_step(step);
    }
    t.lo = lo; t.hi = hi; t.step = hi - lo; t.count = 2;
    return t;
}

void chart_moving_average(const float *in, int n, int window, float *out) {
    if (window < 1) window = 1;
    // Backwards: out[i] reads in[<= i] only, so out may alias in.
    for (int i = n - 1; i >= 0; i--) {
        double sum = 0;
        int cnt = 0;
        for (int k = i; k >= 0 && k > i - window; k--) {
            if (isnan(in[k])) continue;
            sum += in[k];
            cnt++;
        }
        out[i] = cnt ? (float)(sum / cnt) : NAN;
    }
}

int chart_nearest_index(float x, float x0, float x1, int n) {
    if (n <= 0 || x < x0 - 0.5f || x > x1 + 0.5f) return -1;
    if (n == 1 || x1 <= x0) return 0;
    int i = (int)lrintf((x - x0) / (x1 - x0) * (float)(n - 1));
    return DG_CLAMP(i, 0, n - 1);
}

int chart_nearest_point(const float *px, const float *py, int n, float x, float y, float max_d2) {
    int best = -1;
    float bd = max_d2;
    for (int i = 0; i < n; i++) {
        float dx = px[i] - x, dy = py[i] - y, d = dx * dx + dy * dy;
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

int chart_donut_segment(const float *values, int n, float a) {
    double total = 0;
    for (int i = 0; i < n; i++) if (values[i] > 0) total += values[i];
    if (total <= 0) return -1;
    double t = fmod((double)a, 6.283185307179586);
    if (t < 0) t += 6.283185307179586;
    t = t / 6.283185307179586 * total;
    double acc = 0;
    for (int i = 0; i < n; i++) {
        if (values[i] <= 0) continue;
        acc += values[i];
        if (t < acc) return i;
    }
    for (int i = n - 1; i >= 0; i--) if (values[i] > 0) return i;
    return -1;
}

int chart_bucket(float v, float lo, float hi, int n) {
    if (n <= 0) return -1;
    if (hi <= lo) return 0;
    int b = (int)floorf((v - lo) / (hi - lo) * (float)n);
    return DG_CLAMP(b, 0, n - 1);
}

static int floordiv7(int p) { return p >= 0 ? p / 7 : -((-p + 6) / 7); }

int chart_calendar_cell(int i, int days, int last_wd, int *col, int *row) {
    if (days <= 0) { if (col) *col = 0; if (row) *row = 0; return 0; }
    int p0 = last_wd - (days - 1), p = last_wd - (days - 1 - i), plast = last_wd;
    int c0 = floordiv7(p0);
    if (col) *col = floordiv7(p) - c0;
    if (row) *row = ((p % 7) + 7) % 7;
    return floordiv7(plast) - c0 + 1;
}

// ---------------------------------------------------------------- formatting

static const char *group_digits(char *buf, int size, const char *digits, bool neg, const char *frac) {
    char tmp[64];
    int len = (int)strlen(digits), k = 0;
    if (neg) tmp[k++] = '-';
    for (int i = 0; i < len && k < (int)sizeof(tmp) - 2; i++) {
        if (i > 0 && (len - i) % 3 == 0) tmp[k++] = ',';
        tmp[k++] = digits[i];
    }
    tmp[k] = 0;
    snprintf(buf, (size_t)size, "%s%s", tmp, frac ? frac : "");
    return buf;
}

const char *chart_fmt_int(char *buf, int size, double v) {
    if (!isfinite(v)) { snprintf(buf, (size_t)size, "-"); return buf; }
    double r = floor(fabs(v) + 0.5);
    char digits[40];
    snprintf(digits, sizeof(digits), "%.0f", r);
    return group_digits(buf, size, digits, v < 0 && r > 0, NULL);
}

const char *chart_fmt_num(char *buf, int size, double v, int decimals) {
    if (!isfinite(v)) { snprintf(buf, (size_t)size, "-"); return buf; }
    if (decimals <= 0) return chart_fmt_int(buf, size, v);
    char full[64];
    snprintf(full, sizeof(full), "%.*f", decimals, fabs(v));
    char *dot = strchr(full, '.');
    char frac[24] = "";
    if (dot) { dg_strcpy(frac, sizeof(frac), dot); *dot = 0; }
    return group_digits(buf, size, full, v < 0 && fabs(v) >= 0.5 * pow(10.0, -decimals), frac);
}

const char *chart_fmt_pct(char *buf, int size, double frac01, int decimals) {
    char n[48];
    chart_fmt_num(n, sizeof(n), frac01 * 100.0, decimals);
    snprintf(buf, (size_t)size, "%s %%", n);
    return buf;
}

const char *chart_fmt_dist(char *buf, int size, double meters, bool imperial) {
    char n[48];
    if (imperial) {
        double ft = meters * 3.28084;
        if (ft < 1000.0) snprintf(buf, (size_t)size, "%s ft", chart_fmt_int(n, sizeof(n), ft));
        else snprintf(buf, (size_t)size, "%s mi", chart_fmt_num(n, sizeof(n), meters / 1609.344, meters < 160934.4 ? 1 : 0));
    } else {
        if (meters < 1000.0) snprintf(buf, (size_t)size, "%s m", chart_fmt_int(n, sizeof(n), meters));
        else snprintf(buf, (size_t)size, "%s km", chart_fmt_num(n, sizeof(n), meters / 1000.0, meters < 100000.0 ? 1 : 0));
    }
    return buf;
}

const char *chart_fmt_duration(char *buf, int size, double seconds) {
    if (!isfinite(seconds) || seconds < 0) seconds = 0;
    long s = (long)floor(seconds + 0.5);
    long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h > 0) snprintf(buf, (size_t)size, "%ldh %ldm", h, m);
    else if (m > 0) snprintf(buf, (size_t)size, "%ldm %02lds", m, sec);
    else snprintf(buf, (size_t)size, "%lds", sec);
    return buf;
}

const char *chart_fmt_compact(char *buf, int size, double v) {
    double a = fabs(v);
    if (!isfinite(v)) snprintf(buf, (size_t)size, "-");
    else if (a >= 1e6) snprintf(buf, (size_t)size, "%.*fM", a >= 1e7 ? 0 : 1, v / 1e6);
    else if (a >= 1e3) snprintf(buf, (size_t)size, "%.*fk", a >= 1e4 ? 0 : 1, v / 1e3);
    else if (fabs(v - floor(v + 0.5)) < 1e-6) snprintf(buf, (size_t)size, "%.0f", v);
    else snprintf(buf, (size_t)size, "%.3g", v);
    return buf;
}
