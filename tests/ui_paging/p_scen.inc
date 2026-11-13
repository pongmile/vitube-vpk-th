/* ---- paging scenarios ------------------------------------------------------------
 * list_screen.c is #included above, so the lists (g_home, g_search, ...) are
 * visible to the checks. */
extern int h_logged_in, h_ncont, h_home_calls, h_search_calls, h_chan_calls, h_abos_calls;
extern int h_dupe_pages, h_page_rows, h_last_page, h_cancel_next, h_net_tasks;
typedef enum { MORE_OK, MORE_FAIL, MORE_EMPTY, MORE_DUPES } MoreMode;
extern MoreMode h_more_mode;
typedef struct { char token[32]; yt_surface_t surface; char visitor[64]; } ContCall;
extern ContCall h_cont[64];
extern SettingsResult h_settings_result;
extern const char *h_search_text;

static void press_n(unsigned b, int n) { for (int i = 0; i < n; i++) press(b); }
/* Hold a button for `frames` frames. */
static void hold_pad(unsigned b, int frames) {
    ev_add(EV_PAD, T, b, 0, 0, 0, 0, 0, 0);
    ev_add(EV_PAD, T + frames, 0, 0, 0, 0, 0, 0, 0);
    T += frames + 2;
}

static void cont_is(int i, const char *tok, yt_surface_t s, const char *vis) {
    CHECK(i < h_ncont, "continuation call #%d never happened (calls: %d)", i, h_ncont);
    if (i >= h_ncont) return;
    CHECK(strcmp(h_cont[i].token, tok) == 0, "call #%d token '%s' want '%s'", i, h_cont[i].token, tok);
    CHECK(h_cont[i].surface == s, "call #%d surface %d want %d", i, (int)h_cont[i].surface, (int)s);
    CHECK(strcmp(h_cont[i].visitor, vis) == 0, "call #%d visitor '%s' want '%s'", i, h_cont[i].visitor, vis);
}

/* -- 1: Start (signed out) pages by Down on the last row, with its own token + visitor -- */
static void c1_initial(void) {
    CHECK(g_home.count == 76, "home count %d", g_home.count);
    CHECK(strcmp(g_home.continuation, "HT1") == 0 && g_home.cont_surface == YT_SURF_BROWSE_WEB &&
          strcmp(g_home.cont_visitor, "VIS_HOME_1") == 0,
          "home paging state '%s' %d '%s'", g_home.continuation, (int)g_home.cont_surface, g_home.cont_visitor);
    CHECK(h_ncont == 0, "no load before the end: %d", h_ncont);
}
static void c1_on_last(void) { CHECK(g_home.cursor == 75, "cursor %d", g_home.cursor); CHECK(h_ncont == 0, "premature load"); }
static void c1_after(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    cont_is(0, "HT1", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
    CHECK(g_home.count == 106, "count %d", g_home.count);
    CHECK(g_home.cursor == 76, "cursor moves on to the first new row, got %d", g_home.cursor);
    CHECK(strcmp(g_home.continuation, "HT2") == 0 && strcmp(g_home.cont_visitor, "VIS_HOME_1") == 0,
          "next token '%s', visitor '%s' (the original id must be kept)", g_home.continuation, g_home.cont_visitor);
    CHECK(g_home.cont_fails == 0, "fails %d", g_home.cont_fails);
}
static void c1_set80(void) { h_page_rows = 80; }
static void c1_items_max(void) {
    CHECK(g_home.count == 240, "ITEMS_MAX not honoured: count %d", g_home.count);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0,
          "a full list keeps no token: '%s' %d '%s'", g_home.continuation, (int)g_home.cont_surface, g_home.cont_visitor);
}
static void c1_no_more_calls(void) { CHECK(h_ncont == 3, "calls after the list is full: %d", h_ncont); }
static void s1(void) {
    wait(3);
    call(c1_initial);
    press_n(BTN_DOWN, 75);
    call(c1_on_last);
    press(BTN_DOWN);
    wait(2);
    call(c1_after);
    call(c1_set80);
    /* page 2 (76+30=106) -> +80 = 186 -> +80 = capped at 240 */
    press_n(BTN_RIGHT, 21);   /* 106 rows: 76 -> 160 clamps at 105 */
    press(BTN_DOWN); wait(2);
    press_n(BTN_RIGHT, 25);
    press(BTN_DOWN); wait(2);
    call(c1_items_max);
    press_n(BTN_RIGHT, 5);
    press_n(BTN_DOWN, 3); wait(2);
    call(c1_no_more_calls);
}

/* -- 2: a failure keeps the token; a held Down does not retry in a loop; 3 fails end it -- */
static void c2_fail_mode(void) { h_more_mode = MORE_FAIL; }
static void c2_during_hold(void) {
    CHECK(h_ncont == 1, "ONE attempt per press, even with the button held: %d calls", h_ncont);
    CHECK(strcmp(g_home.continuation, "HT1") == 0, "token must survive a failure: '%s'", g_home.continuation);
    CHECK(g_home.cont_fails == 1, "fails %d", g_home.cont_fails);
    CHECK(g_home.count == 76, "count %d", g_home.count);
}
/* Read WHILE the failure toast is up (the toast is modal: this runs from inside
 * its frame loop): the failure must already be counted by then, so nothing that
 * happens during the toast can skip it. */
static void c2_toast(void) {
    seen("Network error"); seen("HTTP error 400");
    CHECK(g_home.cont_fails == 1 && strcmp(g_home.continuation, "HT1") == 0,
          "the failure is counted before its toast: fails %d token '%s'", g_home.cont_fails, g_home.continuation);
}
static void c2_second(void) { CHECK(h_ncont == 2 && g_home.cont_fails == 2 && strcmp(g_home.continuation, "HT1") == 0, "calls %d fails %d", h_ncont, g_home.cont_fails); }
static void c2_ok_mode(void) { h_more_mode = MORE_OK; }
static void c2_recovered(void) {
    CHECK(h_ncont == 3, "calls %d", h_ncont);
    CHECK(g_home.count == 106 && strcmp(g_home.continuation, "HT2") == 0 && g_home.cont_fails == 0,
          "recovery: count %d token '%s' fails %d", g_home.count, g_home.continuation, g_home.cont_fails);
}
static void s2(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c2_fail_mode);
    /* Down held for far longer than the toast and the repeat delay */
    ev_add(EV_PAD, T, BTN_DOWN, 0, 0, 0, 0, 0, 0);
    T += 30; call(c2_toast);
    T += 670; ev_add(EV_PAD, T, 0, 0, 0, 0, 0, 0, 0); T += 2;
    call(c2_during_hold);
    wait(2);
    press(BTN_DOWN); wait(135);  /* a fresh press: a second, single attempt */
    call(c2_second);
    call(c2_ok_mode);
    press(BTN_DOWN); wait(2);
    call(c2_recovered);
}
static void c2b_after3(void) {
    CHECK(h_ncont == 3, "three failures, three calls: %d", h_ncont);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE, "token must be gone after 3 fails: '%s'", g_home.continuation);
}
static void c2b_still3(void) {
    CHECK(h_ncont == 3, "no call without a token: %d", h_ncont);
    CHECK(h_watch_appearances == 0, "three failures are told as failures, not as 'no more videos': %d appearances", h_watch_appearances);
}
/* From here on, count how often "No more videos" comes up (see h_watch). */
static void watch_no_more(void) { h_watch("No more videos"); }
/* Inside the THIRD failure's toast: paging is already over, not still waiting
 * for the toast to end before it is. */
static void c2b_in_toast3(void) {
    seen("Network error");
    CHECK(h_ncont == 3, "calls %d", h_ncont);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE,
          "the third failure ends paging before its toast, not after: '%s'", g_home.continuation);
}
static void s2b(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c2_fail_mode);
    press(BTN_DOWN); wait(135);
    press(BTN_DOWN); wait(135);
    press(BTN_DOWN); wait(20);
    call(c2b_in_toast3);
    wait(114);
    call(c2b_after3);
    press(BTN_DOWN); wait(2);
    call(c2b_still3);
}

/* -- 3: HTTP 200 with 0 rows: clear the token, no error toast -- */
static void c3_mode(void) { h_more_mode = MORE_EMPTY; }
static void c3_check(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    CHECK(g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE, "token not cleared");
    CHECK(g_home.count == 76, "count %d", g_home.count);
    unseen("Network error");
    unseen("Loading more");
}
static void c3_toast_up(void) { seen("No more videos"); }
static void c3_toast_over(void) {
    unseen("No more videos");
    CHECK(h_watch_appearances == 1 && h_watch_frames >= 30 && h_watch_frames <= 90,
          "told once, for a second or so: %d appearances, %d frames", h_watch_appearances, h_watch_frames);
}
static void c3_still_once(void) {
    CHECK(h_watch_appearances == 1, "a second press must not tell again: %d appearances", h_watch_appearances);
}
static void s3(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c3_mode);
    press(BTN_DOWN); wait(2);
    call(c3_check);
    call(c3_toast_up);        /* the dead end is told, in so many words */
    wait(70);                 /* the toast (60 frames) is over */
    call(c3_toast_over);
    press(BTN_DOWN); wait(2);
    call(c3_check);   /* still one call */
    wait(70);
    call(c3_still_once);
}

/* -- 4: a page of repeats is followed, at most two more pages -- */
static void c4_dupes_all(void) { h_more_mode = MORE_DUPES; }
static void c4_check_all(void) {
    CHECK(h_watch_appearances == 0, "repeats with a token behind them are not a dead end: %d appearances", h_watch_appearances);
    CHECK(h_ncont == 3, "1 page + 2 follow-ups = 3 calls, got %d", h_ncont);
    CHECK(g_home.count == 76, "count %d", g_home.count);
    CHECK(strcmp(g_home.continuation, "HT4") == 0, "token after 3 pages '%s'", g_home.continuation);
    cont_is(0, "HT1", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
    cont_is(1, "HT2", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
    cont_is(2, "HT3", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
}
static void c4_check_after_hold(void) { CHECK(h_ncont == 3, "a held Down must not keep following: %d calls", h_ncont); }
static void c4_dupes_once(void) { h_more_mode = MORE_OK; h_dupe_pages = 1; }
static void c4_check_once(void) {
    CHECK(h_ncont == 5, "repeats then fresh rows: 2 more calls, got %d total", h_ncont - 3);
    CHECK(g_home.count == 106, "count %d", g_home.count);
    CHECK(g_home.cursor == 76, "cursor %d", g_home.cursor);
}
static void s4(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(watch_no_more);
    call(c4_dupes_all);
    hold_pad(BTN_DOWN, 600);
    call(c4_check_all);
    call(c4_check_after_hold);
    call(c4_dupes_once);
    press(BTN_DOWN); wait(2);
    call(c4_check_once);
}

/* -- 5: Suche pages; 6: a channel's videos page, and a second channel starts clean -- */
static void c5_search(void) {
    CHECK(g_tab == TAB_SEARCH && g_search.count == 20, "tab %d count %d", (int)g_tab, g_search.count);
    CHECK(strcmp(g_search.continuation, "ST1") == 0 && g_search.cont_surface == YT_SURF_SEARCH && g_search.cont_visitor[0] == 0,
          "search paging state '%s' %d '%s'", g_search.continuation, (int)g_search.cont_surface, g_search.cont_visitor);
}
static void c5_after(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    cont_is(0, "ST1", YT_SURF_SEARCH, "");
    CHECK(g_search.count == 50, "count %d", g_search.count);
}
static void s5(void) {
    wait(3);
    press(BTN_TRI); wait(4);
    call(c5_search);
    press_n(BTN_DOWN, 19);
    press(BTN_DOWN); wait(2);
    call(c5_after);
}
static void c6_in_channel(void) {
    CHECK(g_view[TAB_SEARCH] == VIEW_CHANNEL_DETAIL, "view %d", (int)g_view[TAB_SEARCH]);
    CHECK(g_channel_videos.count == 15 && strcmp(g_channel_videos.continuation, "CT1") == 0 &&
          g_channel_videos.cont_surface == YT_SURF_CHANNEL_TV && g_channel_videos.cont_visitor[0] == 0,
          "channel paging state: count %d '%s' %d", g_channel_videos.count, g_channel_videos.continuation, (int)g_channel_videos.cont_surface);
}
static void c6_after(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    cont_is(0, "CT1", YT_SURF_CHANNEL_TV, "");
    CHECK(g_channel_videos.count == 45 && strcmp(g_channel_videos.continuation, "CT2") == 0, "count %d token '%s'", g_channel_videos.count, g_channel_videos.continuation);
}
static void c6_second_channel(void) {
    CHECK(g_view[TAB_SEARCH] == VIEW_CHANNEL_DETAIL && h_chan_calls == 2, "view %d chan calls %d", (int)g_view[TAB_SEARCH], h_chan_calls);
    CHECK(g_channel_videos.count == 15 && strcmp(g_channel_videos.continuation, "CT1") == 0,
          "the second channel must start from ITS first page, not the first channel's CT2: count %d token '%s'",
          g_channel_videos.count, g_channel_videos.continuation);
}
static void s6(void) {
    wait(3);
    press(BTN_TRI); wait(4);
    press(BTN_SQUARE);          /* channel hits */
    press(BTN_CROSS); wait(3);  /* open channel 0 */
    call(c6_in_channel);
    press_n(BTN_DOWN, 14);
    press(BTN_DOWN); wait(2);
    call(c6_after);
    press(BTN_CIRCLE); wait(2); /* back to the channel list */
    press(BTN_DOWN);
    press(BTN_CROSS); wait(3);  /* open channel 1 */
    call(c6_second_channel);
}

/* -- 7: signed in: Start and Abos are not pageable yet -- */
static void c7_home(void) {
    CHECK(g_home.count == 76 && g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE,
          "signed-in Start must carry no token: count %d '%s'", g_home.count, g_home.continuation);
}
static void c7_abos(void) {
    CHECK(h_abos_calls == 1 && g_abos_feed.count == 8, "abos calls %d count %d", h_abos_calls, g_abos_feed.count);
    CHECK(g_abos_feed.continuation[0] == 0 && g_abos_feed.cont_surface == YT_SURF_NONE,
          "Abos keeps no token while its surface is unverified: '%s'", g_abos_feed.continuation);
    CHECK(h_ncont == 0, "no continuation call: %d", h_ncont);
}
static void s7(void) {
    wait(3);
    call(c7_home);
    press_n(BTN_RIGHT, 19);
    press_n(BTN_DOWN, 3); wait(2);
    call(c7_home);
    press(BTN_R); press(BTN_R); wait(4);   /* Suche, Abos */
    press_n(BTN_RIGHT, 3);
    press_n(BTN_DOWN, 12); wait(2);
    call(c7_abos);
}

/* -- 8: sign-in / sign-out / region change leave no token and no visitor behind -- */
static void c8_signed_out_first(void) {
    CHECK(strcmp(g_home.cont_visitor, "VIS_HOME_1") == 0 && strcmp(g_home.continuation, "HT1") == 0, "setup");
}
static void c8_to_signed_in(void) { h_logged_in = 1; h_settings_result = SETTINGS_SIGNED_IN; }
static void c8_wiped(void) {
    CHECK(g_home.count == 0 && g_home.continuation[0] == 0 && g_home.cont_surface == YT_SURF_NONE && g_home.cont_visitor[0] == 0 && g_home.cont_fails == 0,
          "sign-in must wipe the Start list: count %d '%s' %d '%s'", g_home.count, g_home.continuation, (int)g_home.cont_surface, g_home.cont_visitor);
}
static void c8_back_out(void) { h_logged_in = 0; h_settings_result = SETTINGS_SIGNED_OUT; }
static void c8_reloaded(void) {
    CHECK(h_home_calls == 2 && g_home.count == 76, "home calls %d count %d", h_home_calls, g_home.count);
    CHECK(strcmp(g_home.cont_visitor, "VIS_HOME_2") == 0 && strcmp(g_home.continuation, "HT1") == 0,
          "the reloaded list takes ITS page's visitor: '%s'", g_home.cont_visitor);
}
static void c8_check_call(void) { cont_is(0, "HT1", YT_SURF_BROWSE_WEB, "VIS_HOME_2"); }
static void c8_region(void) { h_settings_result = SETTINGS_RELOAD_ALL; }
static void s8(void) {
    wait(3);
    call(c8_signed_out_first);
    press(BTN_L); wait(3);                  /* Home -> Mehr */
    call(c8_to_signed_in);
    wait(3);                                /* settings input is polled on the Mehr tab */
    call(c8_wiped);
    call(c8_back_out);
    wait(3);
    call(c8_wiped);
    press(BTN_R); wait(5);                  /* Mehr -> Start: reload */
    call(c8_reloaded);
    press_n(BTN_RIGHT, 19);
    press(BTN_DOWN); wait(2);
    call(c8_check_call);
    press(BTN_L); wait(3);
    call(c8_region);
    wait(3);
    call(c8_wiped);
}

/* -- 9: Circle on the spinner keeps the token and is not a failure -- */
static void c9_arm(void) { h_cancel_next = 1; }
static void c9_check(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    CHECK(strcmp(g_home.continuation, "HT1") == 0 && g_home.cont_fails == 0, "token '%s' fails %d", g_home.continuation, g_home.cont_fails);
    unseen("Network error");
}
static void c9_again(void) { CHECK(h_ncont == 2 && g_home.count == 106, "calls %d count %d", h_ncont, g_home.count); }
static void s9(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    call(c9_arm);
    press(BTN_DOWN); wait(2);
    call(c9_check);
    press(BTN_DOWN); wait(2);
    call(c9_again);
}

/* -- 10: the touch trigger: pulling the list past its end -- */
static void c10_check(void) {
    CHECK(h_ncont == 1, "touch at the end must load once, got %d calls", h_ncont);
    CHECK(g_home.count == 106, "count %d", g_home.count);
}
static void s10(void) {
    wait(3);
    press_n(BTN_RIGHT, 19);
    wait(60);                               /* the list eases to the end and rests */
    drag_hold(500, 300, 200, 8, 6);         /* pull up past the end, hold, let go */
    wait(90);
    call(c10_check);
}

/* -- 11: the list does not mix pages: Search token never goes out as Start -- */
static void c11_check(void) {
    CHECK(h_ncont == 1, "calls %d", h_ncont);
    cont_is(0, "HT1", YT_SURF_BROWSE_WEB, "VIS_HOME_1");
    CHECK(strcmp(g_search.continuation, "ST1") == 0, "search token untouched: '%s'", g_search.continuation);
}
static void s11(void) {
    wait(3);
    press(BTN_TRI); wait(4);                /* search first: ST1 pending */
    press(BTN_L); wait(4);                  /* Suche -> Start */
    press_n(BTN_RIGHT, 19);
    press(BTN_DOWN); wait(2);
    call(c11_check);
}
