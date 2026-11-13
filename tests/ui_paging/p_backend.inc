/* ---- yt backend for the paging scenarios -------------------------------------
 * Feeds that page with scripted behaviour. Every call to the continuation is
 * recorded so a scenario can check WHICH token went out with WHICH surface and
 * visitor, and how often. */
int h_logged_in;
typedef enum { MORE_OK, MORE_FAIL, MORE_EMPTY, MORE_DUPES } MoreMode;
MoreMode h_more_mode = MORE_OK;
int h_dupe_pages;          /* in MORE_OK, the first N continuation calls return only repeats */
int h_page_rows = 30;      /* rows per continuation page */
int h_last_page = 100;     /* tokens end after this page number */
extern int h_cancel_next;  /* the next net task is "cancelled with Circle" */
typedef struct { char token[32]; yt_surface_t surface; char visitor[64]; } ContCall;
ContCall h_cont[64];
int h_ncont;
int h_home_calls, h_search_calls, h_chan_calls, h_abos_calls;
static char g_err[96];
const char *h_search_text = "lofi";
int h_toasts;   /* net-task-less: counts nothing; toasts are read off the screen */

static void page_rows(yt_video_list_t *o, int base, int n) {
    o->count = n;
    for (int i = 0; i < n; i++) fake_video(&o->items[i], base + i);
}
static void next_token(yt_video_list_t *o, const char *pfx, int page) {
    if (page < h_last_page) snprintf(o->continuation, sizeof o->continuation, "%s%d", pfx, page + 1);
}

int yt_home_feed(volatile int *cancel, yt_video_list_t *out) {
    (void)cancel;
    h_home_calls++;
    page_rows(out, 0, h_home_count);
    snprintf(out->continuation, sizeof out->continuation, "HT1");
    if (h_logged_in) {
        out->surface = YT_SURF_NONE;   /* signed-in TV: not pageable yet */
    } else {
        out->surface = YT_SURF_BROWSE_WEB;
        snprintf(out->visitor, sizeof out->visitor, "VIS_HOME_%d", h_home_calls);
    }
    return 0;
}
int yt_subscriptions_feed(volatile int *cancel, yt_video_list_t *out) {
    (void)cancel;
    h_abos_calls++;
    page_rows(out, 100, 8);
    snprintf(out->continuation, sizeof out->continuation, "AT1"); /* a token the list must refuse */
    out->surface = YT_SURF_NONE;
    return 0;
}
int yt_search(const char *q, volatile int *c, yt_video_list_t *o, yt_channel_list_t *ch) {
    (void)q; (void)c;
    h_search_calls++;
    page_rows(o, 50000, 20);
    snprintf(o->continuation, sizeof o->continuation, "ST1");
    o->surface = YT_SURF_SEARCH;
    if (ch) {
        ch->count = 2;
        for (int i = 0; i < 2; i++) {
            snprintf(ch->items[i].channel_id, sizeof ch->items[i].channel_id, "UC%022d", i);
            snprintf(ch->items[i].title, sizeof ch->items[i].title, "Channel %d", i);
        }
    }
    return 0;
}
int yt_channel_videos(const char *id, volatile int *c, yt_video_list_t *o, char *t, size_t n) {
    (void)id; (void)c;
    h_chan_calls++;
    page_rows(o, 70000, 15);
    snprintf(o->continuation, sizeof o->continuation, "CT1");
    o->surface = YT_SURF_CHANNEL_TV;
    snprintf(t, n, "Chan");
    return 0;
}
int yt_continue(const char *k, yt_surface_t s, const char *vis, volatile int *c, yt_video_list_t *o) {
    (void)c;
    if (h_ncont < 64) {
        snprintf(h_cont[h_ncont].token, sizeof h_cont[0].token, "%s", k);
        h_cont[h_ncont].surface = s;
        snprintf(h_cont[h_ncont].visitor, sizeof h_cont[0].visitor, "%s", vis ? vis : "");
        h_ncont++;
    }
    memset(o, 0, sizeof *o);
    if (h_more_mode == MORE_FAIL) { snprintf(g_err, sizeof g_err, "HTTP error 400"); return -1; }
    g_err[0] = 0;
    if (h_more_mode == MORE_EMPTY) { o->surface = s; return 0; }
    char pfx[3] = { k[0], k[1], 0 };
    int page = atoi(k + 2);
    int base = (pfx[0] == 'H' ? 1000 : pfx[0] == 'S' ? 60000 : 80000) + page * 1000;
    int dupes = h_more_mode == MORE_DUPES || h_dupe_pages > 0;
    if (h_dupe_pages > 0) h_dupe_pages--;
    if (dupes) page_rows(o, pfx[0] == 'H' ? 0 : pfx[0] == 'S' ? 50000 : 70000, 10);
    else page_rows(o, base, h_page_rows);
    next_token(o, pfx, page);
    o->surface = s;                         /* what the real one stamps */
    snprintf(o->visitor, sizeof o->visitor, "%s", vis ? vis : ""); /* ...and carries forward */
    return 0;
}
int yt_channel_list(volatile int *c, yt_channel_list_t *o) { (void)c; o->count = 0; return 0; }
int yt_subscribe(const char *id, int s, volatile int *c) { (void)id; (void)s; (void)c; return 0; }
const char *yt_last_error(void) { return g_err; }
int yt_oauth_logged_in(void) { return h_logged_in; }
