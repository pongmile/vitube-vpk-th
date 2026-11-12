/* ViTube — number formatting for the Downloads screen. See dl_format.h. */
#include "ui/dl_format.h"

#include <stdio.h>

/* 1024^unit. Unit is B..GB, so at most 2^30: every product below stays far
 * inside int64_t. */
static int64_t unit_size(int unit) {
    return (int64_t)1 << (10 * unit);
}

typedef struct {
    int64_t whole;
    int tenths;      /* 0..9, only meaningful when decimals is set */
    int decimals;    /* 1: print "W.T", 0: print "W" */
} Rendered;

/* `bytes` expressed in `unit`, rounded the way it is printed: to a tenth
 * below 100, to a whole number from 100 up. Rounding can carry (99.96 MB is
 * "100 MB", 9.96 is "10.0"), so the decision about decimals is made on the
 * ROUNDED value, not on the raw one. */
static Rendered render(int64_t bytes, int unit) {
    Rendered r = { 0, 0, 0 };
    if (unit == VT_FMT_B) {
        r.whole = bytes;
        return r;
    }
    int64_t u = unit_size(unit);
    int64_t whole = bytes / u;
    int64_t rem = bytes % u;
    /* rem < 2^30, so rem * 10 cannot overflow. */
    int64_t tenths = (rem * 10 + u / 2) / u;
    if (tenths >= 10) {
        whole += 1;
        tenths = 0;
    }
    if (whole < 100) {
        r.whole = whole;
        r.tenths = (int)tenths;
        r.decimals = 1;
    } else {
        /* From 100 up the integer is rounded on its own: the tenths above
         * already carried into `whole` when they were 9.5 or more, but 100.4
         * must read 100 and 100.5 must read 101. */
        r.whole = bytes / u + (rem * 2 >= u ? 1 : 0);
    }
    return r;
}

int vt_fmt_unit(int64_t bytes) {
    if (bytes < 1024) return VT_FMT_B;
    int unit = bytes < unit_size(VT_FMT_MB) ? VT_FMT_KB
               : bytes < unit_size(VT_FMT_GB) ? VT_FMT_MB : VT_FMT_GB;
    /* 1023.9 KB renders as 1024: the next unit's turn. */
    while (unit < VT_FMT_GB && render(bytes, unit).whole >= 1024) unit++;
    return unit;
}

static const char *const g_unit_name[] = { "B", "KB", "MB", "GB" };

int vt_fmt_bytes_in(char *out, size_t cap, int64_t bytes, int unit,
                    int with_unit, char point) {
    if (!out || cap == 0) return 0;
    if (unit < VT_FMT_B) unit = VT_FMT_B;
    if (unit > VT_FMT_GB) unit = VT_FMT_GB;
    if (bytes < 0) {
        int n = snprintf(out, cap, "?");
        return n < (int)cap ? n : (int)cap - 1;
    }
    if (!point) point = '.';
    Rendered r = render(bytes, unit);
    const char *suffix = with_unit ? " " : "";
    const char *name = with_unit ? g_unit_name[unit] : "";
    int n;
    if (r.decimals)
        n = snprintf(out, cap, "%lld%c%d%s%s", (long long)r.whole, point,
                     r.tenths, suffix, name);
    else
        n = snprintf(out, cap, "%lld%s%s", (long long)r.whole, suffix, name);
    return n < (int)cap ? n : (int)cap - 1;
}

int vt_fmt_bytes(char *out, size_t cap, int64_t bytes, char point) {
    return vt_fmt_bytes_in(out, cap, bytes, vt_fmt_unit(bytes), 1, point);
}

int vt_fmt_percent(int64_t done, int64_t total) {
    if (total <= 0) return -1;
    if (done <= 0) return 0;
    if (done >= total) return 100;
    int64_t pct;
    if (done <= INT64_MAX / 100) {
        pct = done * 100 / total;
    } else {
        /* Sizes in the exabyte range: nothing real, but done * 100 would
         * wrap. The ratio is all that matters and a double has plenty for
         * that. */
        pct = (int64_t)((double)done / (double)total * 100.0);
    }
    /* done < total: never 100, and never negative. */
    return pct > 99 ? 99 : pct < 0 ? 0 : (int)pct;
}

int vt_fmt_speed(char *out, size_t cap, int64_t bytes_per_sec, char point) {
    if (!out || cap == 0) return 0;
    char size[VT_FMT_SIZE_MAX];
    vt_fmt_bytes(size, sizeof size, bytes_per_sec, point);
    int n = snprintf(out, cap, "%s/s", size);
    return n < (int)cap ? n : (int)cap - 1;
}

int vt_fmt_wait(char *out, size_t cap, int seconds) {
    if (!out || cap == 0) return 0;
    int n;
    if (seconds < 0)
        n = snprintf(out, cap, "?");
    else if (seconds < 60)
        n = snprintf(out, cap, "%d s", seconds);
    else if (seconds < 3600)
        n = snprintf(out, cap, "%d min", (seconds + 59) / 60);
    else
        /* Written as quotient plus remainder rather than (s + 3599) / 3600
         * so that INT_MAX stays defined. */
        n = snprintf(out, cap, "%d h", seconds / 3600 +
                                           (seconds % 3600 ? 1 : 0));
    return n < (int)cap ? n : (int)cap - 1;
}
