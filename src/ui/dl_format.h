/* ViTube — number formatting for the Downloads screen: byte sizes, percent,
 * transfer speed, waiting times.
 *
 * Pure C, no psp2, no vita2d, no string catalogue: tests/test_dl_format.c
 * compiles it on the build machine. The one thing that differs between
 * languages is the decimal mark ("36.4 MB" in English, "36,4 MB" in German),
 * and it comes in as a parameter — the caller reads it from the catalogue
 * (S_DL_DECIMAL_MARK) — instead of being a per-language branch in here.
 * Units stay B/KB/MB/GB everywhere: they are what the memory-card and file
 * dialogs of every platform this app's users know print.
 *
 * Sizes are binary (1 KB = 1024 B), like the "29.8 GB" a 32 GB card is
 * reported as by every desktop OS — the Downloads header sits next to that
 * number in the user's head. */
#ifndef VITUBE_UI_DL_FORMAT_H
#define VITUBE_UI_DL_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* Enough for any int64_t size ("8589934591 GB") and "?/s". */
#define VT_FMT_SIZE_MAX 24

enum { VT_FMT_B = 0, VT_FMT_KB, VT_FMT_MB, VT_FMT_GB };

/* The unit vt_fmt_bytes() prints `bytes` in. It is chosen by what the number
 * rounds to, not by where it starts: 1048575 B reads "1.0 MB", never
 * "1024 KB". Negative sizes answer VT_FMT_B. */
int vt_fmt_unit(int64_t bytes);

/* "512 B", "1.5 KB", "36.4 MB", "250 MB", "12.4 GB": one decimal below 100,
 * none from 100 up, where a tenth is noise. A negative size is "unknown" and
 * prints "?". `point` is the decimal mark ('\0' = '.'). Returns the length
 * written, like snprintf, but never more than cap - 1. */
int vt_fmt_bytes(char *out, size_t cap, int64_t bytes, char point);

/* `bytes` in a given unit, for "15.3 of 36.4 MB": the progress is written in
 * the unit of the total (vt_fmt_unit(total)) and only the total carries the
 * suffix. with_unit appends " MB" etc. */
int vt_fmt_bytes_in(char *out, size_t cap, int64_t bytes, int unit,
                    int with_unit, char point);

/* Whole percent, rounded DOWN so that 100 means finished and nothing else
 * (99.9% reads 99). -1 when `total` is not known (<= 0): there is no
 * fraction of an unknown. done <= 0 is 0 and done >= total is 100. */
int vt_fmt_percent(int64_t done, int64_t total);

/* "1.2 MB/s". 0 prints "0 B/s", a negative rate "?/s". */
int vt_fmt_speed(char *out, size_t cap, int64_t bytes_per_sec, char point);

/* A wait, coarsely: "45 s" up to a minute, then whole minutes rounded up
 * ("61 s" is "2 min"), then hours. Rounded up because it is a countdown to
 * something that happens: the thing is due at or before the number shown,
 * never after it. Negative is "?". */
int vt_fmt_wait(char *out, size_t cap, int seconds);

#endif
