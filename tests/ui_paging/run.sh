#!/bin/bash
# Builds the headless paging harness and runs every scenario.
#   CC=gcc bash tests/ui_paging/run.sh   (from the repository root)
#   CC="zig cc" ...                    on a host without gcc
# Extra compiler flags: CFLAGS. LIST_SCREEN_SRC=/abs/path/list_screen.c runs a
# modified copy of list_screen.c (mutation testing).
set -e
CC=${CC:-gcc}
OUT=${OUT:-/tmp/ui_paging}
mkdir -p "$OUT"
H=tests/ui_paging
SRC=""
for f in downloads_screen rows_view ui kinetic_scroll dl_format thai_shape strings \
         strings_table strings_de strings_en strings_es strings_pt strings_it strings_fr; do
    SRC="$SRC src/ui/$f.c"
done
DEFS=()
[ -n "$LIST_SCREEN_SRC" ] && DEFS=("-DLIST_SCREEN_SRC=\"$LIST_SCREEN_SRC\"")
# -w: the real UI sources are built against stand-in SDK headers here, and
# their warnings are the Vita build's business; the scenarios are what is
# being asserted.
$CC -std=gnu11 -g -w -fsanitize=undefined -fno-sanitize-recover=all "${DEFS[@]}" $CFLAGS \
    -Isrc -Iexternal/jansson -I$H/stubs -I$H \
    -o "$OUT/p_harness" $SRC $H/p_main.c $H/p_stubs.c $H/dl_stub.c -pthread -lm
N=$("$OUT/p_harness" 0 2>&1 | sed -n 's/^scenarios: //p')
fail=0
for i in $(seq 1 "${N:-22}"); do
    if ! "$OUT/p_harness" "$i" > "$OUT/s$i.log" 2>&1 || grep -q "FAIL" "$OUT/s$i.log"; then
        echo "=== scenario $i FAILED"; cat "$OUT/s$i.log"; fail=1
    else
        tail -1 "$OUT/s$i.log" | sed "s/^/scenario $i: /"
    fi
done
[ $fail = 0 ] && echo "ALL UI PAGING SCENARIOS PASSED"
exit $fail
