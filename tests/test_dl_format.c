/* Host-side checks on the Downloads screen's number formatting
 * (src/ui/dl_format.c): sizes, percent, speed and waits.
 *
 * What the screen prints from these is what a user judges "is my card full"
 * by, so the edges matter more than the middle: a size that rounds up across
 * a unit boundary must move to the next unit ("1024 KB" is the classic slip),
 * a percentage must say 100 only when the file is complete, and an unknown
 * total must not become a division by zero.
 *
 *   gcc -Isrc -Wall -Wextra -Werror -o /tmp/tdf tests/test_dl_format.c \
 *       src/ui/dl_format.c && /tmp/tdf
 */
#include "ui/dl_format.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

#define KB 1024LL
#define MB (1024LL * 1024)
#define GB (1024LL * 1024 * 1024)

static void bytes_is(int64_t value, const char *want) {
    char buf[VT_FMT_SIZE_MAX];
    vt_fmt_bytes(buf, sizeof buf, value, '.');
    CHECK(strcmp(buf, want) == 0, "bytes(%lld) = \"%s\", want \"%s\"",
          (long long)value, buf, want);
}

static void bytes_comma_is(int64_t value, const char *want) {
    char buf[VT_FMT_SIZE_MAX];
    vt_fmt_bytes(buf, sizeof buf, value, ',');
    CHECK(strcmp(buf, want) == 0, "bytes(%lld) with ',' = \"%s\", want \"%s\"",
          (long long)value, buf, want);
}

static void bytes_in_is(int64_t value, int unit, int with_unit, const char *want) {
    char buf[VT_FMT_SIZE_MAX];
    vt_fmt_bytes_in(buf, sizeof buf, value, unit, with_unit, '.');
    CHECK(strcmp(buf, want) == 0, "bytes_in(%lld, %d, %d) = \"%s\", want \"%s\"",
          (long long)value, unit, with_unit, buf, want);
}

static void speed_is(int64_t value, const char *want) {
    char buf[VT_FMT_SIZE_MAX];
    vt_fmt_speed(buf, sizeof buf, value, '.');
    CHECK(strcmp(buf, want) == 0, "speed(%lld) = \"%s\", want \"%s\"",
          (long long)value, buf, want);
}

static void wait_is(int seconds, const char *want) {
    char buf[VT_FMT_SIZE_MAX];
    vt_fmt_wait(buf, sizeof buf, seconds);
    CHECK(strcmp(buf, want) == 0, "wait(%d) = \"%s\", want \"%s\"", seconds, buf,
          want);
}

static void percent_is(int64_t done, int64_t total, int want) {
    int got = vt_fmt_percent(done, total);
    CHECK(got == want, "percent(%lld, %lld) = %d, want %d", (long long)done,
          (long long)total, got, want);
}

int main(void) {
    /* --- bytes: the small end ------------------------------------------ */
    bytes_is(0, "0 B");
    bytes_is(1, "1 B");
    bytes_is(1023, "1023 B");
    bytes_is(1024, "1.0 KB");
    bytes_is(1536, "1.5 KB");
    /* 1.0449 KB rounds down to 1.0, 1.0508 up to 1.1 */
    bytes_is(1070, "1.0 KB");
    bytes_is(1076, "1.1 KB");

    /* --- rounding at every unit boundary ------------------------------- */
    /* Just under 1 MB must not print as "1024 KB". */
    bytes_is(MB - 1, "1.0 MB");
    bytes_is(MB, "1.0 MB");
    bytes_is(MB - 1024, "1023 KB");
    bytes_is(GB - 1, "1.0 GB");
    bytes_is(GB - 1024, "1.0 GB");          /* 1023.999 MB */
    bytes_is(GB - 600 * KB, "1023 MB");     /* 1023.4 MB stays MB */
    bytes_is(GB, "1.0 GB");

    /* --- the 100 switch from one decimal to none ----------------------- */
    bytes_is(99 * MB + 900 * KB, "99.9 MB");
    /* 99.96 MB rounds to a tenth that carries into 100: no ".0" on it. */
    bytes_is(99 * MB + 983 * KB, "100 MB");
    bytes_is(100 * MB, "100 MB");
    bytes_is(100 * MB + 400 * KB, "100 MB");
    bytes_is(100 * MB + 600 * KB, "101 MB");
    bytes_is(250 * MB, "250 MB");

    /* --- typical values ------------------------------------------------- */
    bytes_is(36 * MB + 400 * KB, "36.4 MB");
    bytes_is(15 * MB + 300 * KB, "15.3 MB");
    bytes_is(12 * GB + 400 * MB, "12.4 GB");
    bytes_is((int64_t)(29.8 * GB), "29.8 GB");
    /* the card from the header example: 32 GB marketing, 29.8 GiB real */
    bytes_is(32000000000LL, "29.8 GB");
    bytes_comma_is(36 * MB + 400 * KB, "36,4 MB");
    bytes_comma_is(250 * MB, "250 MB");
    bytes_comma_is(512, "512 B");

    /* --- huge and unknown ---------------------------------------------- */
    bytes_is(1024 * GB, "1024 GB");         /* there is no TB: GB just grows */
    bytes_is(9000 * GB, "9000 GB");
    bytes_is(INT64_MAX, "8589934592 GB");
    bytes_is(-1, "?");
    bytes_is(INT64_MIN, "?");
    CHECK(vt_fmt_unit(-5) == VT_FMT_B, "unit of a negative size");
    CHECK(vt_fmt_unit(0) == VT_FMT_B, "unit of 0");
    CHECK(vt_fmt_unit(1023) == VT_FMT_B, "unit of 1023");
    CHECK(vt_fmt_unit(1024) == VT_FMT_KB, "unit of 1024");
    CHECK(vt_fmt_unit(MB - 1) == VT_FMT_MB, "unit of MB-1 rounds up to MB");
    CHECK(vt_fmt_unit(MB) == VT_FMT_MB, "unit of MB");
    CHECK(vt_fmt_unit(GB - 1) == VT_FMT_GB, "unit of GB-1 rounds up to GB");
    CHECK(vt_fmt_unit(5 * GB) == VT_FMT_GB, "unit of 5 GB");

    /* --- a size in somebody else's unit: "15.3 of 36.4 MB" -------------- */
    bytes_in_is(15 * MB + 300 * KB, VT_FMT_MB, 0, "15.3");
    bytes_in_is(36 * MB + 400 * KB, VT_FMT_MB, 1, "36.4 MB");
    bytes_in_is(0, VT_FMT_MB, 0, "0.0");
    bytes_in_is(300 * KB, VT_FMT_MB, 0, "0.3");
    bytes_in_is(250 * MB, VT_FMT_MB, 0, "250");
    bytes_in_is(2 * GB, VT_FMT_MB, 0, "2048");
    bytes_in_is(7, VT_FMT_B, 1, "7 B");
    bytes_in_is(-3, VT_FMT_MB, 0, "?");
    /* an out-of-range unit is clamped, not read off the end of the table */
    bytes_in_is(5 * MB, 99, 1, "0.0 GB");
    bytes_in_is(5 * MB, -4, 1, "5242880 B");

    /* --- percent -------------------------------------------------------- */
    percent_is(0, 100, 0);
    percent_is(1, 100, 1);
    percent_is(42, 100, 42);
    percent_is(15 * MB + 300 * KB, 36 * MB + 400 * KB, 42);
    /* rounded DOWN: 99.9% must not read 100 */
    percent_is(999, 1000, 99);
    percent_is(9999, 10000, 99);
    percent_is(1000, 1000, 100);
    percent_is(1001, 1000, 100);            /* server sent more than it said */
    percent_is(5, 1000, 0);                 /* 0.5% */
    /* nothing known about the total: no percentage, never a division by 0 */
    percent_is(0, 0, -1);
    percent_is(500, 0, -1);
    percent_is(500, -1, -1);
    percent_is(-5, 100, 0);                 /* nonsense in, 0 out */
    /* sizes where done * 100 would overflow 64 bits */
    percent_is(INT64_MAX - 1, INT64_MAX, 99);
    percent_is(INT64_MAX / 2, INT64_MAX, 50);
    percent_is(INT64_MAX, INT64_MAX, 100);

    /* --- speed ---------------------------------------------------------- */
    speed_is(0, "0 B/s");
    speed_is(900, "900 B/s");
    speed_is(1200 * KB, "1.2 MB/s");
    speed_is(1258291, "1.2 MB/s");
    speed_is(MB, "1.0 MB/s");
    speed_is(-1, "?/s");
    {
        char buf[VT_FMT_SIZE_MAX];
        vt_fmt_speed(buf, sizeof buf, 1536 * KB, ',');
        CHECK(strcmp(buf, "1,5 MB/s") == 0, "speed with a comma: \"%s\"", buf);
    }

    /* --- waits ---------------------------------------------------------- */
    wait_is(0, "0 s");
    wait_is(1, "1 s");
    wait_is(59, "59 s");
    wait_is(60, "1 min");
    wait_is(61, "2 min");                   /* rounded UP: it is a countdown */
    wait_is(120, "2 min");
    wait_is(3599, "60 min");
    wait_is(3600, "1 h");
    wait_is(3601, "2 h");
    wait_is(2147483647, "596524 h");
    wait_is(-1, "?");

    /* --- buffers: truncation must be clean and always terminated -------- */
    {
        char tiny[6];
        int n = vt_fmt_bytes(tiny, sizeof tiny, 36 * MB + 400 * KB, '.');
        CHECK(n == 5 && strcmp(tiny, "36.4 ") == 0,
              "truncated to 6 bytes: n=%d \"%s\"", n, tiny);
        char one[1];
        n = vt_fmt_bytes(one, sizeof one, 5, '.');
        CHECK(n == 0 && one[0] == '\0', "cap 1 gives the empty string");
        n = vt_fmt_bytes(NULL, 0, 5, '.');
        CHECK(n == 0, "NULL/0 buffer is a no-op");
        n = vt_fmt_wait(tiny, 3, 125);
        CHECK(n == 2 && strcmp(tiny, "3 ") == 0, "wait truncated: \"%s\"", tiny);
        n = vt_fmt_speed(tiny, 4, 5 * MB, '.');
        CHECK(n == 3 && strcmp(tiny, "5.0") == 0, "speed truncated: \"%s\"", tiny);
    }

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL DL FORMAT TESTS PASSED\n");
    return 0;
}
