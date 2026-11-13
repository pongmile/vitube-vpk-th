/* ======================= scenarios added by the testing agent ======================= */

/* -- 13: fling into the end with a FAILING backend: one attempt per gesture,
 *        nothing repeats by itself while the list rests, the third failure ends paging -- */
static void c13_fail(void) { h_more_mode = MORE_FAIL; }
static void c13_ok(void) { h_more_mode = MORE_OK; }
static void c13_n1(void) { CHECK(h_ncont == 1, "one attempt after the first fling, got %d", h_ncont);
    CHECK(strcmp(g_home.continuation, "HT1") == 0 && g_home.cont_fails == 1, "token '%s' fails %d", g_home.continuation, g_home.cont_fails); }
static void c13_n1_still(void) { CHECK(h_ncont == 1, "a resting list must not retry by itself: %d attempts", h_ncont); }
static void c13_n2(void) { CHECK(h_ncont == 2 && g_home.cont_fails == 2 && strcmp(g_home.continuation, "HT1") == 0, "second fling: calls %d fails %d token '%s'", h_ncont, g_home.cont_fails, g_home.continuation); }
static void c13_n3(void) { CHECK(h_ncont == 3, "third fling: calls %d", h_ncont);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0, "the third failure ends paging: '%s'", g_home.continuation); }
static void c13_n3_still(void) { CHECK(h_ncont == 3, "no token, no call: %d", h_ncont); }
static void s13(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    wait(60);
    call(c13_fail);
    drag(500, 420, 120, 4); wait(260);      /* the failure toast is modal: 120 frames */
    call(c13_n1);
    wait(200);
    call(c13_n1_still);
    drag(500, 420, 120, 4); wait(260);
    call(c13_n2);
    drag(500, 420, 120, 4); wait(260);
    call(c13_n3);
    drag(500, 420, 120, 4); wait(260);
    call(c13_n3_still);
}

/* -- 14: the Start list and Suche keep their own paging state across tab switches -- */
static void c14_a(void) { CHECK(h_ncont == 1 && g_home.count == 106 && strcmp(g_home.continuation, "HT2") == 0, "start: calls %d count %d token '%s'", h_ncont, g_home.count, g_home.continuation);
    CHECK(strcmp(g_search.continuation, "ST1") == 0 && g_search.count == 20, "search: '%s' %d", g_search.continuation, g_search.count); }
static void c14_b(void) { CHECK(h_ncont == 2, "calls %d", h_ncont); cont_is(0, "HT1", YT_SURF_BROWSE_WEB, "VIS_HOME_1"); cont_is(1, "HT2", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
    CHECK(strcmp(g_search.continuation, "ST1") == 0, "search token untouched: '%s'", g_search.continuation); }
static void c14_c(void) { CHECK(h_ncont == 3, "calls %d", h_ncont); cont_is(2, "ST1", YT_SURF_SEARCH, ""); CHECK(strcmp(g_home.continuation, "HT3") == 0, "home token '%s'", g_home.continuation); }
static void s14(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    press(BTN_DOWN); wait(2);               /* Start: HT1 -> 106 rows, token HT2 */
    press(BTN_TRI); wait(4);                /* Suche: ST1 pending */
    call(c14_a);
    press(BTN_L); wait(4);                  /* back to Start */
    press_n(BTN_RIGHT, 9);                  /* 106 -> end */
    press(BTN_DOWN); wait(2);
    call(c14_b);
    press(BTN_R); wait(4);                  /* to Suche */
    press_n(BTN_DOWN, 19);
    press(BTN_DOWN); wait(2);
    call(c14_c);
}

/* -- 15: a region change empties Suche (its token and visitor with it) and resets the failure count -- */
static void c15_pre(void) { CHECK(h_ncont == 1 && g_home.cont_fails == 1, "setup: calls %d fails %d", h_ncont, g_home.cont_fails);
    CHECK(strcmp(g_search.continuation, "ST1") == 0, "setup: search token '%s'", g_search.continuation); }
static void c15_to_more(void) { h_settings_result = SETTINGS_RELOAD_ALL; }
static void c15_post(void) {
    CHECK(g_search.count == 0 && g_search.continuation[0] == 0 && g_search.cont_surface == YT_SURF_NONE && g_search.cont_visitor[0] == 0 && g_search.cont_fails == 0,
          "region change must empty Suche with its paging state: count %d '%s'", g_search.count, g_search.continuation);
    CHECK(g_home.count == 0 && g_home.continuation[0] == 0 && g_home.cont_fails == 0, "Start too: count %d '%s' fails %d", g_home.count, g_home.continuation, g_home.cont_fails);
}
static void c15_reload(void) { CHECK(g_home.count == 76 && g_home.cont_fails == 0 && strcmp(g_home.continuation, "HT1") == 0 && strcmp(g_home.cont_visitor, "VIS_HOME_2") == 0,
    "reloaded Start: count %d fails %d token '%s' visitor '%s'", g_home.count, g_home.cont_fails, g_home.continuation, g_home.cont_visitor); }
static void s15(void) {
    wait(3);
    press(BTN_TRI); wait(4);                /* Suche first (ST1) */
    press(BTN_L); wait(4);                  /* Start */
    press_n(BTN_RIGHT, 19);
    call(c13_fail);
    press(BTN_DOWN); wait(135);             /* one failed attempt */
    call(c15_pre);
    call(c13_ok);
    press(BTN_L); wait(3);                  /* to Mehr */
    call(c15_to_more);
    wait(3);
    call(c15_post);
    press(BTN_R); wait(5);                  /* Mehr -> Start: reload */
    call(c15_reload);
}

/* -- 16: ITEMS_MAX reached exactly (76 + 128 + 36 = 240), and one short of it -- */
static void c16_set128(void) { h_page_rows = 128; }
static void c16_set36(void) { h_page_rows = 36; }
static void c16_set35(void) { h_page_rows = 35; }
static void c16_set1(void) { h_page_rows = 1; }
static void c16_204(void) { CHECK(g_home.count == 204 && strcmp(g_home.continuation, "HT2") == 0, "after the big page: count %d token '%s'", g_home.count, g_home.continuation); }
static void c16_exact(void) {
    CHECK(g_home.count == 240, "count %d", g_home.count);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0, "full list keeps no token: '%s'", g_home.continuation);
    CHECK(h_ncont == 2, "calls %d", h_ncont);
}
static void c16_still(void) { CHECK(h_ncont == 2, "no call from a full list: %d", h_ncont); }
static void c16_short(void) {
    CHECK(g_home.count == 239 && strcmp(g_home.continuation, "HT3") == 0 && h_ncont == 2, "239 rows keep paging: count %d token '%s' calls %d", g_home.count, g_home.continuation, h_ncont);
}
static void c16_last(void) {
    CHECK(g_home.count == 240 && g_home.continuation[0] == 0 && h_ncont == 3, "the 240th row ends it: count %d '%s' calls %d", g_home.count, g_home.continuation, h_ncont);
}
static void s16(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c16_set128);
    press(BTN_DOWN); wait(2);
    call(c16_204);
    call(c16_set36);
    press_n(BTN_RIGHT, 52);
    press(BTN_DOWN); wait(2);
    call(c16_exact);
    press_n(BTN_RIGHT, 5);
    press_n(BTN_DOWN, 3); wait(2);
    call(c16_still);
}
static void s16b(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c16_set128);
    press(BTN_DOWN); wait(2);
    call(c16_204);
    call(c16_set35);
    press_n(BTN_RIGHT, 52);
    press(BTN_DOWN); wait(2);
    call(c16_short);
    call(c16_set1);
    press_n(BTN_RIGHT, 12);
    press(BTN_DOWN); wait(2);
    call(c16_last);
}

/* -- 17: a channel's paging never reaches the search list or the next channel, even after going back and forth -- */
static void c17_a(void) { CHECK(g_view[TAB_SEARCH] == VIEW_CHANNEL_DETAIL && strcmp(g_channel_videos.continuation, "CT2") == 0 && g_channel_videos.count == 45, "chan A paged: '%s' %d view %d", g_channel_videos.continuation, g_channel_videos.count, (int)g_view[TAB_SEARCH]);
    CHECK(strcmp(g_search.continuation, "ST1") == 0, "search token '%s'", g_search.continuation); }
static void c17_back(void) { CHECK(g_view[TAB_SEARCH] == VIEW_VIDEOS, "view %d", (int)g_view[TAB_SEARCH]); }
static void c17_b(void) { CHECK(h_ncont == 2, "calls %d", h_ncont); cont_is(1, "ST1", YT_SURF_SEARCH, ""); }
static void c17_reopen(void) { CHECK(g_view[TAB_SEARCH] == VIEW_CHANNEL_DETAIL && g_channel_videos.count == 15 && strcmp(g_channel_videos.continuation, "CT1") == 0 && g_channel_videos.cont_fails == 0,
    "re-opened channel starts at its first page: count %d '%s' fails %d view %d", g_channel_videos.count, g_channel_videos.continuation, g_channel_videos.cont_fails, (int)g_view[TAB_SEARCH]); }
static void s17(void) {
    wait(3);
    press(BTN_TRI); wait(4);                /* Suche, ST1 */
    press(BTN_SQUARE);                      /* channel hits */
    press(BTN_CROSS); wait(3);              /* open channel 0 */
    press_n(BTN_DOWN, 14); press(BTN_DOWN); wait(2);   /* CT1 -> 45 rows, CT2 */
    call(c17_a);
    press(BTN_CIRCLE); wait(2);             /* back to the channel hits */
    press(BTN_SQUARE); wait(2);             /* back to the video results */
    call(c17_back);
    press_n(BTN_DOWN, 19); press(BTN_DOWN); wait(2);   /* Suche pages with ITS token */
    call(c17_b);
    press(BTN_SQUARE); wait(2);
    press(BTN_CROSS); wait(3);              /* the same channel again */
    call(c17_reopen);
}

/* -- 18: held Down at the end for 120 frames: a failing backend gets one attempt; a working one is not asked twice -- */
static void c18_fail_one(void) { CHECK(h_ncont == 1 && g_home.cont_fails == 1, "failing backend: %d attempts, fails %d", h_ncont, g_home.cont_fails); }
static void s18(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c13_fail);
    hold_pad(BTN_DOWN, 120);
    call(c18_fail_one);
}
static void c18_ok_one(void) { CHECK(h_ncont == 1 && g_home.count == 106, "working backend: %d calls, %d rows", h_ncont, g_home.count); }
static void s18b(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    hold_pad(BTN_DOWN, 120);
    call(c18_ok_one);
}
static void c18_tiny(void) { CHECK(h_ncont >= 2 && h_ncont <= (240 - 76) / 3 + 2, "tiny pages held for 600 frames: %d calls (bounded by ITEMS_MAX)", h_ncont); CHECK(g_home.count <= 240, "count %d", g_home.count); }
static void c18_tiny_set(void) { h_page_rows = 3; }
static void s18c(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c18_tiny_set);
    hold_pad(BTN_DOWN, 600);
    call(c18_tiny);
}

/* -- 19: a stale token (HTTP 200, 0 rows): ends paging once, with no toast, however long Down is held -- */
static void c19_empty(void) { h_more_mode = MORE_EMPTY; }
static void c19_ended(void) {
    CHECK(h_ncont == 1 && g_home.continuation[0] == 0, "calls %d token '%s'", h_ncont, g_home.continuation); unseen("Network error");
    CHECK(h_watch_appearances == 1, "Down held for 120 frames: told %d times, want once", h_watch_appearances);
    CHECK(h_watch_frames >= 30 && h_watch_frames <= 90, "the toast is a short one: %d frames", h_watch_frames);
}
static void s19(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c19_empty);
    hold_pad(BTN_DOWN, 120);
    call(c19_ended);
}

/* -- 20: the feed ends (the last real page has rows and no token): the list stops asking, with no spinner and no toast -- */
static void c20_last_page(void) { h_last_page = 2; }
static void c20_a(void) { CHECK(h_ncont == 1 && g_home.count == 106 && strcmp(g_home.continuation, "HT2") == 0, "after page 2: calls %d count %d token '%s'", h_ncont, g_home.count, g_home.continuation); }
static void c20_b(void) {
    CHECK(h_ncont == 2 && g_home.count == 136, "after the last page: calls %d count %d", h_ncont, g_home.count);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0 && g_home.cont_fails == 0,
          "a page with no token ends the paging: '%s' %d", g_home.continuation, (int)g_home.cont_surface);
}
static void c20_c(void) {
    CHECK(h_ncont == 2, "no call once the feed has ended: %d", h_ncont); unseen("Network error"); unseen("Loading more");
    CHECK(h_watch_appearances == 0, "a last page that brought rows is not announced: %d appearances", h_watch_appearances);
}
static void s20(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c20_last_page);
    press(BTN_DOWN); wait(2);
    call(c20_a);
    press_n(BTN_RIGHT, 8);
    press(BTN_DOWN); wait(2);
    call(c20_b);
    press_n(BTN_RIGHT, 8);
    press_n(BTN_DOWN, 4); wait(2);
    drag(500, 420, 120, 4); wait(120);
    call(c20_c);
}

/* -- 21: a dead end is told once however long, and however often, the user keeps
 *        asking: Down held for 600 frames, flings into the end, more presses -- */
static void c21_once(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    CHECK(h_watch_appearances == 1, "told %d times, want once", h_watch_appearances);
}
static void s21(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c19_empty);
    hold_pad(BTN_DOWN, 600);
    call(c21_once);
    drag(500, 420, 120, 4); wait(120);
    press_n(BTN_DOWN, 4); wait(70);
    drag(500, 420, 120, 4); wait(120);
    call(c21_once);
}

/* -- 22: the last page is nothing but repeats and has no token: the same dead end,
 *        told once. (With a token still behind it, repeats are followed instead:
 *        scenario 4.) -- */
static void c22_last_is_dupes(void) { h_last_page = 1; h_more_mode = MORE_DUPES; }
static void c22_a(void) {
    CHECK(h_ncont == 1, "one page asked for, and it has no token to follow: %d calls", h_ncont);
    CHECK(g_home.count == 76 && g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0,
          "nothing added, paging over: count %d token '%s'", g_home.count, g_home.continuation);
    CHECK(h_watch_appearances == 1, "told %d times, want once", h_watch_appearances);
}
static void s22(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c22_last_is_dupes);
    press(BTN_DOWN); wait(2);
    call(c3_toast_up);
    wait(70);
    call(c22_a);
    press_n(BTN_DOWN, 3); wait(70);
    call(c22_a);
}

/* -- 23: each list is told its own dead end; one list's ending does not silence the next -- */
static void c23_start(void) { CHECK(h_ncont == 1 && h_watch_appearances == 1 && g_home.continuation[0] == 0, "Start: calls %d told %d token '%s'", h_ncont, h_watch_appearances, g_home.continuation); }
static void c23_search(void) {
    CHECK(h_ncont == 2 && h_watch_appearances == 2, "Suche: calls %d told %d (want 2 and 2)", h_ncont, h_watch_appearances);
    cont_is(1, "ST1", YT_SURF_SEARCH, "");
    CHECK(g_search.continuation[0] == 0 && g_search.cont_surface == YT_SURF_NONE, "Suche keeps no token: '%s'", g_search.continuation);
}
static void c23_back(void) { CHECK(h_ncont == 2 && h_watch_appearances == 2, "back on Start: calls %d told %d", h_ncont, h_watch_appearances); }
static void s23(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c19_empty);
    press(BTN_DOWN); wait(70);              /* Start: dead end */
    call(c23_start);
    press(BTN_TRI); wait(4);                /* Suche: ST1 pending */
    press_n(BTN_DOWN, 19);
    press(BTN_DOWN); wait(70);
    call(c23_search);
    press(BTN_L); wait(4);                  /* back to Start, at its last row */
    press_n(BTN_DOWN, 3); wait(70);
    call(c23_back);
}
