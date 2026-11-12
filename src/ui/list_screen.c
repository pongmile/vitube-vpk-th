/* ViTube — app shell: Start / Suche / Abos / Downloads / Mehr tabs, channel
 * drill-in.
 *
 * There is no in-app "quit" — the Vita's HOME button suspends/closes the
 * app the normal way, so this loop only ever returns by falling out of
 * main() when the process is torn down externally. */
#include "ui/list_screen.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <strings.h> /* strcasecmp, for the alphabetical channel sort */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "dl/dl.h"
#include "net/netwatch.h"
#include "ui/net_task.h"
#include "ui/downloads_screen.h"
#include "ui/kinetic_scroll.h"
#include "ui/player_screen.h"
#include "ui/rows_view.h"
#include "store.h"
#include "ui/settings_screen.h"
#include "ui/strings.h"
#include "ui/text_input.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "yt/innertube.h"
#include "yt/oauth.h"

#define ITEMS_MAX 240
#define ROW_H 96
#define CHANNEL_ROW_H 72
#define TOP_BAR_H 44
#define TAB_BAR_H 34
#define LIST_Y (TOP_BAR_H + TAB_BAR_H)
#define HINT_H 36
/* The band the lists scroll in: from under the tab bar to the hint bar. */
#define LIST_BOTTOM (VT_SCREEN_H - HINT_H)
#define LIST_H (LIST_BOTTOM - LIST_Y)
/* How far Left/Right page the cursor. */
#define VISIBLE_ROWS 4

/* Where a list is scrolled to, by the pixel (see kinetic_scroll.h), plus
 * what list_scroll_step() last wrote back into the list's row-granular
 * `cursor` and `top` — which is how a change made to either from outside
 * the scroll code is noticed:
 *
 * - `top` stays the index of the first row in view, kept up to date every
 *   frame. A `top` written from outside is honoured as "scroll there".
 *   Code that refills a list from scratch (a new search, a reload, a
 *   re-sort) also calls list_scroll_reset(): `top = 0` alone goes unnoticed
 *   when `top` already was 0.
 * - A `cursor` that moved since the last frame (buttons, a tap, a reset) is
 *   eased into view. Touch scrolling deliberately does not move the cursor
 *   while the list is in motion; see list_scroll_step(). */
typedef struct {
    VtKineticScroll kinetic;
    int synced_top;
    int synced_cursor;
} ListScroll;

/* Back to the top, at rest, for a list whose rows were all replaced. A list
 * nudged by less than one row (or still coasting near the top) has `top` 0
 * already, so writing `top = 0` changes nothing list_scroll_step() can see:
 * the new first row would be drawn up to a row's height under the tab bar,
 * and a fling or drag left running on the old rows would go on over the
 * new ones. All-zero is what a fresh list starts as (see VtKineticScroll),
 * and matches the `cursor = 0, top = 0` the caller writes next to it. */
static void list_scroll_reset(ListScroll *s) {
    memset(s, 0, sizeof *s);
}

/* TAB_DL is the one tab with a single view and none of the lists below: its
 * rows, scrolling, menu and input are all in downloads_screen.c. */
typedef enum {
    TAB_HOME = 0, TAB_SEARCH, TAB_ABOS, TAB_DL, TAB_MORE, TAB_COUNT
} AppTab;
/* Every tab is in one of three views, and both Suche and Abos start on their
 * video list with Square flipping to the channels: the search hits, or the
 * subscribed channels. The channel detail is entered from either tab's
 * channel view — which is also how "where does Circle go back to" is
 * remembered: it returns to VIEW_CHANNELS of the tab it was opened in, so no
 * separate origin state is needed. */
typedef enum {
    VIEW_VIDEOS = 0,
    VIEW_CHANNELS,
    VIEW_CHANNEL_DETAIL,
    /* Both of these are "the record behind this tab" and both are on START:
     * what has been watched, and the queries that got here. Square is left
     * to what it already meant — a second view of the same results — which
     * is why neither of these is a stop in its cycle. */
    VIEW_HISTORY,
    VIEW_SEARCHES
} ListView;

typedef struct {
    yt_video_t items[ITEMS_MAX];
    int count;
    char continuation[YT_CONTINUATION_MAX];
    int cursor;
    int top; /* first row in view — see ListScroll */
    int loaded; /* has a fetch ever completed (successfully or not) */
    ListScroll scroll;
} VideoListState;

typedef struct {
    yt_channel_t items[YT_LIST_MAX];
    int count;
    char continuation[YT_CONTINUATION_MAX];
    int cursor;
    int top; /* first row in view — see ListScroll */
    int loaded;
    ListScroll scroll;
} ChannelListState;

static AppTab g_tab = TAB_HOME;
/* Every tab opens on its video list — for Abos that is the "new videos"
 * feed, which is what the tab is usually opened for; Square switches to the
 * channel list. It also means the feed is already loaded by the time the
 * channel list is sorted by most recent upload, which is derived from it. */
static ListView g_view[TAB_COUNT] = { VIEW_VIDEOS, VIEW_VIDEOS, VIEW_VIDEOS,
                                      VIEW_VIDEOS, VIEW_VIDEOS };
static VideoListState g_home, g_search, g_abos_feed, g_channel_videos;
/* Filled from the store when the view is entered, so the rest of this
 * screen — cursor, scrolling, touch, playing — needs to know nothing
 * about where those rows came from. */
static VideoListState g_history;
/* Cursor over the stored searches shown on an otherwise empty Suche tab. */
static int g_recent_cursor;
/* Which store revision g_history was copied at. */
static unsigned g_history_revision;
static ChannelListState g_channels, g_search_channels;
static char g_search_query[256];
/* The channel the detail view is showing — a copy, not a pointer, because the
 * list it came from (search hits, or the subscriptions list this very screen
 * rewrites when you subscribe) can move under it. */
static yt_channel_t g_channel;
static volatile int g_fetch_cancel;
/* vt_netwatch_generation() as of the last Start load; see the empty-Start
 * retry in list_screen_run(). */
static unsigned g_home_net_generation;

static void video_list_absorb(VideoListState *list, const yt_video_list_t *page) {
    for (int i = 0; i < page->count && list->count < ITEMS_MAX; i++) {
        int duplicate = 0;
        for (int k = 0; k < list->count; k++) {
            if (strcmp(list->items[k].video_id, page->items[i].video_id) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) list->items[list->count++] = page->items[i];
    }
    snprintf(list->continuation, sizeof list->continuation, "%s",
             page->continuation);
}

static void channel_list_absorb(ChannelListState *list,
                                const yt_channel_list_t *page) {
    for (int i = 0; i < page->count && list->count < YT_LIST_MAX; i++) {
        int duplicate = 0;
        for (int k = 0; k < list->count; k++) {
            if (strcmp(list->items[k].channel_id, page->items[i].channel_id) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) list->items[list->count++] = page->items[i];
    }
    snprintf(list->continuation, sizeof list->continuation, "%s",
             page->continuation);
}

/* --- fetching ------------------------------------------------------- */

typedef struct {
    const char *query;
    const char *continuation;
    volatile int *cancel;
    yt_video_list_t page;
    yt_channel_list_t channels;
} SearchTask;

static int search_task(void *opaque) {
    SearchTask *task = opaque;
    if (task->continuation && task->continuation[0])
        return yt_continue(task->continuation, task->cancel, &task->page);
    /* One request for both: the search response carries the channel hits
     * alongside the videos, and IT_LIST_FIELDS does not filter them out. */
    return yt_search(task->query, task->cancel, &task->page, &task->channels);
}

static void run_search(const char *query) {
    g_fetch_cancel = 0;
    SearchTask task = { query, NULL, &g_fetch_cancel, {0} };
    int ret = vt_ui_run_net_task(vt_str(S_SEARCHING), search_task, &task,
                                 &g_fetch_cancel);
    g_search.loaded = 1;
    if (g_fetch_cancel) return;
    if (ret < 0) {
        vt_ui_toast(vt_str(S_SEARCH_FAILED), vt_str(S_CHECK_NETWORK), 120);
        return;
    }
    g_search.count = 0;
    g_search.cursor = 0;
    g_search.top = 0;
    list_scroll_reset(&g_search.scroll);
    snprintf(g_search_query, sizeof g_search_query, "%s", query);
    /* Only queries that actually returned: a list of failed attempts is
     * not a list of what the user was looking for. */
    vt_store_search_add(query);
    video_list_absorb(&g_search, &task.page);
    /* Channel hits get their own view rather than being mixed into the video
     * rows: the two row types are 96 and 72 px tall and the whole list
     * geometry — draw, touch, scroll — is one multiplication by ROW_H. */
    g_search_channels.count = 0;
    g_search_channels.cursor = 0;
    g_search_channels.top = 0;
    list_scroll_reset(&g_search_channels.scroll);
    g_search_channels.loaded = 1;
    channel_list_absorb(&g_search_channels, &task.channels);
    g_view[TAB_SEARCH] = g_search.count == 0 && g_search_channels.count > 0
                             ? VIEW_CHANNELS
                             : VIEW_VIDEOS;
}

static void load_more_search(void) {
    if (!g_search.continuation[0] || g_search.count >= ITEMS_MAX) return;
    g_fetch_cancel = 0;
    SearchTask task = { NULL, g_search.continuation, &g_fetch_cancel, {0} };
    int ret = vt_ui_run_net_task(vt_str(S_LOADING_MORE), search_task, &task,
                                 &g_fetch_cancel);
    g_search.continuation[0] = '\0';
    if (ret == 0 && !g_fetch_cancel) video_list_absorb(&g_search, &task.page);
}

typedef struct {
    volatile int *cancel;
    yt_video_list_t page;
} SimpleFetchTask;

static int home_fetch_task(void *opaque) {
    SimpleFetchTask *task = opaque;
    return yt_home_feed(task->cancel, &task->page);
}

static void run_home(void) {
    g_fetch_cancel = 0;
    SimpleFetchTask task = { &g_fetch_cancel, {0} };
    int ret = vt_ui_run_net_task(vt_str(S_LOADING_HOME), home_fetch_task,
                                 &task, &g_fetch_cancel);
    /* Only a fetch that brought videos counts as loaded. A failed or
     * cancelled one (Wi-Fi not associated yet at launch, a timeout, Circle
     * on the spinner) used to latch loaded=1 too, and since !loaded is the
     * only refetch trigger, Start then said "restart the app" for the rest
     * of the session. An empty 200 is a failure as well: it_list_call
     * records it in yt_last_error(). */
    g_home.loaded = !g_fetch_cancel && ret == 0 && task.page.count > 0;
    /* Whatever woke or reconnected during this load has been tried with
     * already; only a later one is a reason to try again by itself. */
    g_home_net_generation = vt_netwatch_generation();
    if (g_fetch_cancel) return;
    g_home.count = 0;
    g_home.cursor = 0;
    g_home.top = 0;
    list_scroll_reset(&g_home.scroll);
    if (ret == 0) video_list_absorb(&g_home, &task.page);
    if (!g_home.loaded)
        vt_ui_toast(vt_str(S_NO_VIDEOS_LOADED),
                    yt_last_error()[0] ? yt_last_error()
                                       : vt_str(S_NETWORK_ERROR), 150);
}

static int abos_feed_fetch_task(void *opaque) {
    SimpleFetchTask *task = opaque;
    return yt_subscriptions_feed(task->cancel, &task->page);
}

static void run_abos_feed(void) {
    g_fetch_cancel = 0;
    SimpleFetchTask task = { &g_fetch_cancel, {0} };
    int ret = vt_ui_run_net_task(vt_str(S_LOADING_NEW_VIDEOS),
                                 abos_feed_fetch_task, &task, &g_fetch_cancel);
    /* A transport failure or a cancel leaves the list unloaded so the next
     * visit retries; "not signed in" and a genuinely empty feed are answers
     * and stay loaded, or the toast would repeat on every tab pass. */
    g_abos_feed.loaded = !g_fetch_cancel && ret >= 0;
    if (g_fetch_cancel) return;
    if (ret == -2) {
        g_abos_feed.loaded = 1;
        vt_ui_toast(vt_str(S_NOT_SIGNED_IN), vt_str(S_SIGN_IN_MORE_TAB), 120);
        return;
    }
    if (ret < 0 || task.page.count == 0) {
        char detail[160];
        snprintf(detail, sizeof detail, "%s",
                 yt_last_error()[0] ? yt_last_error() : vt_str(S_UNKNOWN_ERROR));
        vt_ui_toast(vt_str(S_NO_NEW_VIDEOS), detail, 150);
        return;
    }
    g_abos_feed.count = 0;
    g_abos_feed.cursor = 0;
    g_abos_feed.top = 0;
    list_scroll_reset(&g_abos_feed.scroll);
    video_list_absorb(&g_abos_feed, &task.page);
}

typedef struct {
    volatile int *cancel;
    yt_channel_list_t page;
} ChannelsFetchTask;

static int channels_fetch_task(void *opaque) {
    ChannelsFetchTask *task = opaque;
    return yt_channel_list(task->cancel, &task->page);
}

/* Channel-list order. Alphabetical is the default because it makes a channel
 * findable; "newest upload first" answers the other question — who posted
 * something recently. The recency is taken from the subscription feed the app
 * already loads, matched by channel name: asking YouTube for each channel's
 * latest upload would be one request per channel (40+ here). Channels with no
 * video in the feed keep their alphabetical order after the rest. */
/* RECENT is 0 so that a store with nothing in it yet yields the default
 * this list has always had, without store.c needing to know what the
 * numbers mean. */
typedef enum {
    CH_SORT_RECENT = VT_CHANNEL_SORT_RECENT,
    CH_SORT_ALPHA = VT_CHANNEL_SORT_ALPHA
} ChannelSort;
/* Newest upload first by default: the question this list usually answers is
 * "who posted something?", not "where is channel X?". START switches to
 * alphabetical for the times it is the other way round, and that choice
 * survives a restart now. */
static ChannelSort channels_sort(void) {
    return (ChannelSort)vt_store_channel_sort();
}

static int channel_recency(const yt_channel_t *c) {
    int best = INT_MAX;
    for (int i = 0; i < g_abos_feed.count; i++) {
        const yt_video_t *v = &g_abos_feed.items[i];
        /* Match on the channel id, not the display name: the feed and the
         * subscriptions list spell the same channel differently often enough
         * that name matching quietly found nothing and left the order
         * unchanged. Fall back to the name only when no id came through. */
        int same = v->channel_id[0] && c->channel_id[0]
                       ? strcmp(v->channel_id, c->channel_id) == 0
                       : strcmp(v->channel, c->title) == 0;
        if (!same) continue;
        int age = v->age_min < 0 ? INT_MAX : v->age_min;
        if (age < best) best = age;
    }
    return best;
}

static void sort_channels(void) {
    /* Insertion sort, stable: equal keys (notably every channel absent from
     * the feed, which all share INT_MAX) keep the order they came in — and
     * the incoming order is alphabetical, so those stay alphabetical. */
    for (int a = 1; a < g_channels.count; a++) {
        yt_channel_t tmp = g_channels.items[a];
        int key = channels_sort() == CH_SORT_RECENT ? channel_recency(&tmp) : 0;
        int b = a - 1;
        while (b >= 0) {
            const yt_channel_t *o = &g_channels.items[b];
            int cmp = channels_sort() == CH_SORT_RECENT
                          ? (channel_recency(o) <= key ? -1 : 1)
                          : (strcasecmp(o->title, tmp.title) <= 0 ? -1 : 1);
            if (cmp < 0) break;
            g_channels.items[b + 1] = g_channels.items[b];
            b--;
        }
        g_channels.items[b + 1] = tmp;
    }
    g_channels.cursor = 0;
    g_channels.top = 0;
    list_scroll_reset(&g_channels.scroll);
}

static void run_channels(void) {
    g_fetch_cancel = 0;
    ChannelsFetchTask task = { &g_fetch_cancel, {0} };
    int ret = vt_ui_run_net_task(vt_str(S_LOADING_CHANNELS),
                                 channels_fetch_task, &task, &g_fetch_cancel);
    /* Same rule as run_abos_feed: only a transport failure or a cancel is
     * worth retrying on the next visit. */
    g_channels.loaded = !g_fetch_cancel && ret >= 0;
    if (g_fetch_cancel) return;
    if (ret == -2) {
        g_channels.loaded = 1;
        vt_ui_toast(vt_str(S_NOT_SIGNED_IN), vt_str(S_SIGN_IN_MORE_TAB), 120);
        return;
    }
    if (ret < 0 || task.page.count == 0) {
        char detail[160];
        snprintf(detail, sizeof detail, "%s",
                 yt_last_error()[0] ? yt_last_error() : vt_str(S_UNKNOWN_ERROR));
        vt_ui_toast(vt_str(S_NO_CHANNELS_FOUND), detail, 150);
        return;
    }
    g_channels.count = 0;
    g_channels.cursor = 0;
    g_channels.top = 0;
    list_scroll_reset(&g_channels.scroll);
    channel_list_absorb(&g_channels, &task.page);
    /* Recency comes from the subscription feed, so it has to be there before
     * the list can be ordered by it — one extra request when the Abos tab is
     * opened, which is the price of the default order. */
    if (channels_sort() == CH_SORT_RECENT && !g_abos_feed.loaded)
        run_abos_feed();
    sort_channels();
}

typedef struct {
    const char *channel_id;
    volatile int *cancel;
    yt_video_list_t page;
    char title[YT_CHANNEL_MAX];
} ChannelVideosTask;

static int channel_videos_task(void *opaque) {
    ChannelVideosTask *task = opaque;
    return yt_channel_videos(task->channel_id, task->cancel, &task->page,
                             task->title, sizeof task->title);
}

static void run_channel_detail(const yt_channel_t *channel) {
    g_fetch_cancel = 0;
    ChannelVideosTask task = { channel->channel_id, &g_fetch_cancel, {0}, "" };
    int ret = vt_ui_run_net_task(vt_str(S_LOADING_CHANNEL_VIDEOS),
                                 channel_videos_task, &task, &g_fetch_cancel);
    if (g_fetch_cancel) return;
    if (ret < 0 || task.page.count == 0) {
        vt_ui_toast(vt_str(S_CHANNEL_UNAVAILABLE), yt_last_error(), 150);
        return;
    }
    /* Only channel_id and title are read from here on, so a search hit works
     * exactly like a subscriptions entry — no second detail view. Keeping the
     * avatar too lets a new subscription be spliced into the Abos list below
     * without refetching it. */
    g_channel = *channel;
    if (task.title[0])
        snprintf(g_channel.title, sizeof g_channel.title, "%s", task.title);
    g_channel_videos.count = 0;
    g_channel_videos.cursor = 0;
    g_channel_videos.top = 0;
    list_scroll_reset(&g_channel_videos.scroll);
    g_channel_videos.loaded = 1;
    video_list_absorb(&g_channel_videos, &task.page);
    g_view[g_tab] = VIEW_CHANNEL_DETAIL;
}

/* --- subscribe / unsubscribe ------------------------------------------- */

/* Position of `channel_id` in the subscribed list, or -1. That list is only
 * as complete as what has been loaded: it is empty until the Abos tab is
 * opened and capped at YT_LIST_MAX, so a channel opened straight from search
 * can read as "not subscribed". Subscribing twice is a no-op server side, so
 * the worst case is one redundant request and a label that corrects itself. */
static int subscription_index(const char *channel_id) {
    for (int i = 0; i < g_channels.count; i++)
        if (strcmp(g_channels.items[i].channel_id, channel_id) == 0) return i;
    return -1;
}

typedef struct {
    const char *channel_id;
    int subscribe;
    volatile int *cancel;
} SubscribeTask;

static int subscribe_task(void *opaque) {
    SubscribeTask *task = opaque;
    return yt_subscribe(task->channel_id, task->subscribe, task->cancel);
}

static void toggle_subscription(void) {
    int index = subscription_index(g_channel.channel_id);
    int subscribe = index < 0;
    g_fetch_cancel = 0;
    SubscribeTask task = { g_channel.channel_id, subscribe, &g_fetch_cancel };
    int ret = vt_ui_run_task(vt_str(subscribe ? S_SUBSCRIBING : S_UNSUBSCRIBING),
                             subscribe_task, &task, &g_fetch_cancel);
    if (g_fetch_cancel) return;
    if (ret == -2) {
        vt_ui_toast(vt_str(S_NOT_SIGNED_IN), vt_str(S_SIGN_IN_MORE_TAB), 120);
        return;
    }
    if (ret < 0) {
        vt_ui_toast(vt_str(subscribe ? S_SUBSCRIBE_FAILED
                                     : S_UNSUBSCRIBE_FAILED),
                    yt_last_error(), 150);
        return;
    }
    /* Keep the subscribed list in step instead of refetching it: both the
     * hint bar below and the Abos tab read it. */
    if (subscribe) {
        if (g_channels.count < YT_LIST_MAX)
            g_channels.items[g_channels.count++] = g_channel;
    } else {
        for (int i = index; i + 1 < g_channels.count; i++)
            g_channels.items[i] = g_channels.items[i + 1];
        g_channels.count--;
        if (g_channels.cursor >= g_channels.count && g_channels.cursor > 0)
            g_channels.cursor--;
        if (g_channels.top > g_channels.cursor) g_channels.top = g_channels.cursor;
    }
    vt_ui_toast(vt_str(subscribe ? S_SUBSCRIBED : S_UNSUBSCRIBED),
                g_channel.title, 60);
}

/* --- main loop ---------------------------------------------------------- */

/* Copies the stored history into a normal list so every other part of this
 * screen can stay unaware of it. Cheap enough to redo on each entry (30 rows
 * at most, no network), which also means a video watched since last time is
 * simply there. */
static void load_history(void) {
    memset(&g_history, 0, sizeof g_history);
    g_history_revision = vt_store_revision();
    int n = vt_store_history_count();
    for (int i = 0; i < n && g_history.count < ITEMS_MAX; i++)
        g_history.items[g_history.count++] = *vt_store_history_at(i);
    g_history.loaded = 1;
}

/* The last queries are reachable two ways, and both land here: as the empty
 * Suche tab's content, which is where a first search starts from, and as a
 * view on Start, which is the only way back to them once a search has been
 * run — the tab stops being empty at that point and used to stay that way
 * until the app was restarted. */
static int showing_recent_searches(void) {
    if (g_tab != TAB_SEARCH) return 0;
    if (g_view[TAB_SEARCH] == VIEW_SEARCHES) return 1;
    /* An empty results view shows them too: that is where a first search
     * starts from, and there is nothing else to put there. */
    return g_view[TAB_SEARCH] == VIEW_VIDEOS && g_search.count == 0;
}

/* Every vt_ui_run_task-backed fetch runs its own frames for seconds; without
 * this the pre-spinner `previous` makes a button still held when it ends read
 * as a fresh press, and a finger still down becomes a tap. */
static void resync_input(SceCtrlData *previous) {
    sceCtrlPeekBufferPositive(0, previous, 1);
    ui_touch_reset();
}

static VideoListState *active_video_list(void) {
    if (g_tab == TAB_MORE) return NULL; /* settings rows, not videos */
    if (g_tab == TAB_DL) return NULL;   /* download rows, not videos */
    if (g_view[g_tab] == VIEW_CHANNEL_DETAIL) return &g_channel_videos;
    if (g_view[g_tab] == VIEW_HISTORY) return &g_history;
    if (g_view[g_tab] == VIEW_SEARCHES) return NULL; /* query strings */
    if (g_view[g_tab] == VIEW_CHANNELS) return NULL; /* channel rows instead */
    if (g_tab == TAB_HOME) return &g_home;
    if (g_tab == TAB_SEARCH) return &g_search;
    return &g_abos_feed;
}

/* The channel rows on screen, or NULL when this view shows videos. */
static ChannelListState *active_channel_list(void) {
    if (g_view[g_tab] != VIEW_CHANNELS) return NULL;
    if (g_tab == TAB_SEARCH) return &g_search_channels;
    if (g_tab == TAB_ABOS) return &g_channels;
    return NULL;
}

/* Signing out has to empty what the account filled. Each list caches a
 * `loaded` flag and a tab switch only refetches when that flag is clear, so
 * without this the previous account's subscription feed and channel list stay
 * on screen — and stay reachable — until the app is restarted.
 *
 * g_channel_videos and g_channel are deliberately left alone: they are
 * overwritten by run_channel_detail() before that view can be entered again,
 * so nothing stale can surface, and wiping them would blank a channel page
 * the user may currently be reading from the (anonymous) search side. */
static void drop_lists(int include_search) {
    memset(&g_home, 0, sizeof g_home);
    memset(&g_abos_feed, 0, sizeof g_abos_feed);
    memset(&g_channels, 0, sizeof g_channels);
    g_view[TAB_ABOS] = VIEW_VIDEOS;
    /* Search survives a logout — those results are anonymous and losing them
     * for signing out would be a surprise — but not a region change, which
     * puts them in the wrong language along with everything else. */
    if (include_search) {
        memset(&g_search, 0, sizeof g_search);
        memset(&g_search_channels, 0, sizeof g_search_channels);
        g_search_query[0] = '\0';
        g_recent_cursor = 0;
        g_view[TAB_SEARCH] = VIEW_VIDEOS;
        /* The store may have just been emptied under this copy. */
        load_history();
    }
    /* Those rows' avatars and thumbnails sit in the image cache and are just
     * as much account data as the rows were. */
    vt_thumbs_clear();
}

static void drop_account_data(void) {
    drop_lists(0);
    /* Nothing is refetched here. Signing in and out both happen on the Mehr
     * tab, so the next L1/R1 is what puts a list back on screen — and it
     * loads what the new state calls for, rather than spinning up a fetch
     * behind a settings screen the user is still looking at. */
}

/* --- scrolling ---------------------------------------------------------- */

/* Rows [*first, *last] lie wholly inside the band as it is drawn right now.
 * The band (430 px) is taller than any row, so for a non-empty list there is
 * always at least one. */
static void full_rows(const ListScroll *s, int row_h, int count, int *first,
                      int *last) {
    int off = vt_kinetic_draw_offset(&s->kinetic);
    int f = off > 0 ? (off + row_h - 1) / row_h : 0;
    int l = (off + LIST_H) / row_h - 1;
    if (l > count - 1) l = count - 1;
    if (f > l) f = l;
    if (f < 0) f = 0;
    *first = f;
    *last = l < f ? f : l;
}

/* No part of the cursor's row is in the band. A row that is merely cut off
 * at an edge still counts as on screen: with touch, the selection is "the
 * row I tapped, which a second tap opens", and nudging the list by a few
 * pixels must not quietly hand that to the neighbouring row. */
static int cursor_off_screen(const ListScroll *s, int cursor, int row_h) {
    int off = vt_kinetic_draw_offset(&s->kinetic);
    return (cursor + 1) * row_h <= off || cursor * row_h >= off + LIST_H;
}

/* Moves an off-screen cursor to the nearest row that is wholly on screen.
 * Returns 1 if it had to. */
static int cursor_onto_screen(const ListScroll *s, int *cursor, int count,
                              int row_h) {
    if (!cursor_off_screen(s, *cursor, row_h)) return 0;
    int first, last;
    full_rows(s, row_h, count, &first, &last);
    *cursor = *cursor < first ? first : last;
    return 1;
}

/* The first press of a direction after the list was flung away from the
 * cursor picks a row on screen instead of moving relative to one that is
 * not: otherwise Down would scroll the list all the way back to where the
 * fling started, which reads as the fling being undone. Returns 1 if it did
 * (and the press is used up).
 *
 * Not while an eased scroll runs: a row on its way into view is the D-pad's
 * own motion, and pressing again during it must simply move on from it. */
static int cursor_into_view(ListScroll *s, int *cursor, int count, int row_h) {
    if (s->kinetic.mode == VT_KINETIC_EASING) return 0;
    return cursor_onto_screen(s, cursor, count, row_h);
}

/* One frame of a list's scrolling: honours outside writes to `top` and
 * `cursor`, feeds the touch event, advances the motion, and writes `top`
 * back. Called every frame the list is on screen, after the buttons have
 * moved the cursor and before drawing.
 *
 * Returns the row a tap landed on, or -1. What a tap does is the caller's
 * (select, or open the row that already was selected — as before).
 * *settled gets the VT_KINETIC_SETTLED* bits of a touch-driven motion that
 * came to rest this frame.
 *
 * The cursor is left alone while the finger or a fling is moving the list.
 * Dragging the selection along, or snapping the list back to it, is exactly
 * the "the list fights my finger" this replaced. Only once everything is
 * still is a cursor that ended up off screen moved onto it, so the next
 * button press starts from something visible (and Cross cannot play a video
 * the user is not looking at).
 *
 * That makes "a list at rest has its cursor on screen" an invariant, and it
 * is enforced as one: a resting list with its cursor elsewhere can only have
 * been put there from outside, so the cursor's row is eased in. */
static int list_scroll_step(ListScroll *s, int *cursor, int *top, int count,
                            int row_h, unsigned touch_flags,
                            const UiTouchEvent *touch, unsigned *settled) {
    VtKineticScroll *k = &s->kinetic;
    uint64_t now = sceKernelGetProcessTimeWide();
    vt_kinetic_set_bounds(k, (float)LIST_H, (float)(count * row_h));
    if (*top != s->synced_top) vt_kinetic_jump(k, (float)(*top * row_h));

    int tapped = -1, tap_y = 0;
    if (vt_kinetic_touch(k, touch_flags, touch->x, touch->y - LIST_Y, now,
                         &tap_y) &&
        tap_y >= 0 && tap_y / row_h < count)
        tapped = tap_y / row_h;
    /* A release that stopped the list (no fling) settles right here; pull
     * the cursor on screen BEFORE the invariant below looks at it, or that
     * would ease the list straight back to the old cursor — undoing the
     * drag the moment the finger lifts. */
    *settled = vt_kinetic_take_settled(k);
    if (*settled) cursor_onto_screen(s, cursor, count, row_h);

    if (*cursor != s->synced_cursor ||
        (k->mode == VT_KINETIC_IDLE && cursor_off_screen(s, *cursor, row_h)))
        vt_kinetic_ensure_visible(k, (float)(*cursor * row_h),
                                  (float)((*cursor + 1) * row_h));
    vt_kinetic_update(k, now);

    /* ...and a fling or spring that ran out during this update. */
    unsigned late = vt_kinetic_take_settled(k);
    if (late) cursor_onto_screen(s, cursor, count, row_h);
    *settled |= late;
    s->synced_cursor = *cursor;

    int off = vt_kinetic_draw_offset(k);
    int t = off > 0 ? off / row_h : 0;
    if (t > count - 1) t = count - 1;
    if (t < 0) t = 0;
    *top = s->synced_top = t;
    return tapped;
}

/* The tab bar's labels and count bubbles. The Downloads tab shows how many
 * items are unfinished and not stopped, so a download left running is
 * visible from every other tab. */
static void tab_bar_content(const char **labels, int *badges) {
    for (int i = 0; i < TAB_COUNT; i++) badges[i] = 0;
    labels[TAB_HOME] = vt_str(S_TAB_HOME);
    labels[TAB_SEARCH] = vt_str(S_TAB_SEARCH);
    labels[TAB_ABOS] = vt_str(S_TAB_SUBS);
    labels[TAB_DL] = vt_str(S_TAB_DOWNLOADS);
    labels[TAB_MORE] = vt_str(S_TAB_MORE);
    badges[TAB_DL] = dl_active_count();
}

/* Leaves the current tab for `target` and loads what that tab needs. L1/R1,
 * a tap on the tab bar and "Open in Downloads" all come through here, so none
 * of them can differ in what entering a tab does. */
static void switch_tab(AppTab target, SceCtrlData *previous) {
    if (g_view[g_tab] == VIEW_CHANNEL_DETAIL)
        g_view[g_tab] = VIEW_CHANNELS;
    g_tab = target;
    int fetched = 0;
    if (g_tab == TAB_HOME && !g_home.loaded) {
        run_home();
        fetched = 1;
    }
    if (g_tab == TAB_ABOS && g_view[TAB_ABOS] == VIEW_CHANNELS &&
        !g_channels.loaded) {
        run_channels();
        fetched = 1;
    }
    if (g_tab == TAB_ABOS && g_view[TAB_ABOS] == VIEW_VIDEOS &&
        !g_abos_feed.loaded) {
        run_abos_feed();
        fetched = 1;
    }
    if (g_tab == TAB_DL) downloads_screen_enter();
    if (fetched) resync_input(previous);
}

void list_screen_run(void) {
    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    VtNavRepeat nav = {0};
    ui_touch_reset();

    run_home();

    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        unsigned held = ctrl.buttons;
        previous = ctrl;
        UiTouchEvent touch;
        unsigned touch_flags = ui_touch_poll(&touch);
        /* A picker inside the Mehr tab owns the buttons while it is open;
         * without this, Triangle would open the search keyboard and L1/R1
         * would change tabs straight through it. */
        int modal = (g_tab == TAB_MORE && settings_screen_modal()) ||
                    downloads_menu_active();

        if (!modal && (pressed & SCE_CTRL_TRIANGLE)) {
            char query[256];
            int got = ui_text_input(vt_str(S_SEARCH_PROMPT), g_search_query,
                                    query, sizeof query);
            sceCtrlPeekBufferPositive(0, &previous, 1);
            ui_touch_reset();
            if (got == 1) {
                g_tab = TAB_SEARCH;
                run_search(query);
                /* Again after the search, not only after the keyboard: the
                 * load can now sit on the reconnect screen for most of a
                 * minute (ui/net_task.c), and a finger resting on the
                 * screen when the results arrive would otherwise go down as
                 * a fresh touch and its release tap row 0 — the selected
                 * row, so the tap plays it. */
                resync_input(&previous);
            }
        }
        if (!modal && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)))
            switch_tab((AppTab)((g_tab + (pressed & SCE_CTRL_RTRIGGER
                                              ? 1 : TAB_COUNT - 1)) %
                                TAB_COUNT),
                       &previous);
        /* A tap on a tab. Judged by where the finger came DOWN: it may have
         * drifted a few pixels by the time it lifted, and the bar is thin. A
         * touch that began on the bar is outside every list's viewport, so
         * the scrolling code never saw it. */
        if (!modal && (touch_flags & UI_TOUCH_EVENT_TAP)) {
            const char *labels[TAB_COUNT];
            int badges[TAB_COUNT];
            tab_bar_content(labels, badges);
            int hit = vt_ui_tab_hit(labels, badges, TAB_COUNT, touch.down_x,
                                    touch.down_y);
            if (hit >= 0 && hit != (int)g_tab)
                switch_tab((AppTab)hit, &previous);
        }
        if (g_view[g_tab] == VIEW_CHANNEL_DETAIL) {
            /* Circle returns to the channel list of the tab this was opened
             * from — the search hits, or the subscriptions. */
            if (pressed & SCE_CTRL_CIRCLE) g_view[g_tab] = VIEW_CHANNELS;
            /* Square is unused in this view in both tabs, and is already the
             * app's contextual secondary action elsewhere. */
            if (pressed & SCE_CTRL_SQUARE) {
                toggle_subscription();
                sceCtrlPeekBufferPositive(0, &previous, 1);
            }
        } else if (g_tab == TAB_HOME) {
            /* Same Square as the other tabs, flipping Start between the feed
             * and what has actually been watched on this console. */
            if (pressed & SCE_CTRL_START) {
                g_view[TAB_HOME] = g_view[TAB_HOME] == VIEW_HISTORY
                                       ? VIEW_VIDEOS : VIEW_HISTORY;
                if (g_view[TAB_HOME] == VIEW_HISTORY) load_history();
            }
        } else if (g_tab == TAB_SEARCH) {
            if (pressed & SCE_CTRL_START) {
                g_view[TAB_SEARCH] = g_view[TAB_SEARCH] == VIEW_SEARCHES
                                         ? VIEW_VIDEOS : VIEW_SEARCHES;
                if (g_view[TAB_SEARCH] == VIEW_SEARCHES) g_recent_cursor = 0;
            }
            if (pressed & SCE_CTRL_SQUARE)
                g_view[TAB_SEARCH] = g_view[TAB_SEARCH] == VIEW_VIDEOS
                                         ? VIEW_CHANNELS
                                         : VIEW_VIDEOS;
        } else if (g_tab == TAB_ABOS) {
            if (pressed & SCE_CTRL_SQUARE) {
                g_view[TAB_ABOS] = g_view[TAB_ABOS] == VIEW_CHANNELS
                                       ? VIEW_VIDEOS
                                       : VIEW_CHANNELS;
                int fetched = 0;
                if (g_view[TAB_ABOS] == VIEW_CHANNELS && !g_channels.loaded) {
                    run_channels();
                    fetched = 1;
                }
                if (g_view[TAB_ABOS] == VIEW_VIDEOS && !g_abos_feed.loaded) {
                    run_abos_feed();
                    fetched = 1;
                }
                if (fetched) resync_input(&previous);
            }
            if (g_view[TAB_ABOS] == VIEW_CHANNELS && (pressed & SCE_CTRL_START)) {
                vt_store_set_channel_sort(channels_sort() == CH_SORT_ALPHA
                                              ? CH_SORT_RECENT
                                              : CH_SORT_ALPHA);
                /* Sorting by recency reads the subscription feed, so make
                 * sure it is there before sorting by it. */
                if (channels_sort() == CH_SORT_RECENT && !g_abos_feed.loaded) {
                    run_abos_feed();
                    resync_input(&previous);
                }
                sort_channels();
                vt_ui_toast(vt_str(channels_sort() == CH_SORT_RECENT
                                       ? S_CHANNELS_RECENT
                                       : S_CHANNELS_ALPHA),
                            NULL, 60);
            }
        }
        /* On a video row SELECT is that row's download menu: of the buttons,
         * only this one means nothing else in any of the video lists (Square
         * flips views or subscribes, START switches to the history, Triangle
         * searches). The Mehr tab is still one L1/R1 away, and SELECT still
         * goes there from every screen that has no row to ask about. On the
         * Downloads tab the same button opens the menu of the selected
         * download, which downloads_screen_input() handles itself.
         *
         * A selection a drag or fling carried off screen is brought back
         * first and uses up the press, as Cross does for playing. */
        if (!modal && (pressed & SCE_CTRL_SELECT)) {
            VideoListState *rows = active_video_list();
            if (rows && rows->count > 0) {
                if (!cursor_into_view(&rows->scroll, &rows->cursor,
                                      rows->count, ROW_H))
                    downloads_menu_open_video(&rows->items[rows->cursor]);
            } else if (!(g_tab == TAB_DL && downloads_screen_count() > 0)) {
                /* A shortcut to the tab, not to the login screen: signing in
                 * is one row inside it now, next to the settings that used
                 * to have no home at all. */
                if (g_view[g_tab] == VIEW_CHANNEL_DETAIL)
                    g_view[g_tab] = VIEW_CHANNELS;
                g_tab = TAB_MORE;
            }
        }

        if (g_view[g_tab] == VIEW_HISTORY &&
            g_history_revision != vt_store_revision())
            load_history();

        /* The empty Start screen is what a load leaves behind once
         * net_task.c's retries ran out (or Circle stopped them). It used to
         * stay there until someone read the hint and pressed Cross; now the
         * next wake from suspend or the network coming back loads it again
         * by itself — through the same spinner, so it is visible. */
        if (vt_netwatch_generation() != g_home_net_generation) {
            g_home_net_generation = vt_netwatch_generation();
            if (g_tab == TAB_HOME && g_view[TAB_HOME] == VIEW_VIDEOS &&
                g_home.count == 0) {
                run_home();
                resync_input(&previous);
                /* `pressed` is from before the load; a Cross in it would
                 * play the first row of the list that just arrived. */
                continue;
            }
        }

        unsigned direction = vt_nav_repeat(&nav, pressed, held, ctrl.lx, ctrl.ly);
        ChannelListState *channels = active_channel_list();
        VideoListState *videos = active_video_list();

        if (downloads_menu_active()) {
            /* Before the tab it was opened over: while it is up it owns the
             * buttons and the touch panel, wherever it came from. */
            DlMenuResult chosen = downloads_menu_input(pressed, direction,
                                                       touch_flags, &touch);
            if (chosen == DLM_RESYNC) resync_input(&previous);
            else if (chosen == DLM_OPEN_DOWNLOADS)
                switch_tab(TAB_DL, &previous);
        } else if (g_tab == TAB_DL) {
            if (downloads_screen_input(pressed, direction, touch_flags, &touch,
                                       LIST_Y, LIST_BOTTOM))
                resync_input(&previous);
        } else if (g_tab == TAB_MORE) {
            SettingsResult settings = settings_screen_input(pressed, direction);
            if (pressed & SCE_CTRL_CROSS) {
                /* Whatever that row did may have run its own input loop and
                 * its own frames. */
                sceCtrlPeekBufferPositive(0, &previous, 1);
                ui_touch_reset();
            }
            if (settings == SETTINGS_SIGNED_OUT) {
                drop_account_data();
            } else if (settings == SETTINGS_SIGNED_IN) {
                /* Same reasoning as drop_account_data(): clear, don't fetch.
                 * What was on screen was the anonymous feed and an empty
                 * subscription list; both are wrong now. */
                drop_lists(0);
            } else if (settings == SETTINGS_RELOAD_ALL) {
                drop_lists(1);
            }
        } else if (showing_recent_searches() && vt_store_search_count() > 0) {
            int last = vt_store_search_count() - 1;
            if (g_recent_cursor > last) g_recent_cursor = last;
            if ((direction & SCE_CTRL_UP) && g_recent_cursor > 0)
                g_recent_cursor--;
            if ((direction & SCE_CTRL_DOWN) && g_recent_cursor < last)
                g_recent_cursor++;
            if (pressed & SCE_CTRL_CROSS) {
                /* Copied out first: run_search() records the query, which
                 * reorders the very list this pointer came from. */
                char query[256];
                snprintf(query, sizeof query, "%s",
                         vt_store_search_at(g_recent_cursor));
                g_view[TAB_SEARCH] = VIEW_VIDEOS;
                run_search(query);
                sceCtrlPeekBufferPositive(0, &previous, 1);
                ui_touch_reset();
            }
        } else if (channels && channels->count > 0) {
            ChannelListState *ch = channels;
            if (direction &&
                cursor_into_view(&ch->scroll, &ch->cursor, ch->count,
                                 CHANNEL_ROW_H))
                direction = 0;
            if (direction & SCE_CTRL_UP && ch->cursor > 0) ch->cursor--;
            if (direction & SCE_CTRL_DOWN && ch->cursor < ch->count - 1)
                ch->cursor++;
            unsigned settled;
            int row = list_scroll_step(&ch->scroll, &ch->cursor, &ch->top,
                                       ch->count, CHANNEL_ROW_H, touch_flags,
                                       &touch, &settled);
            if (row >= 0) {
                if (row == ch->cursor) {
                    run_channel_detail(&ch->items[row]);
                    /* Touch too, as for every load here: see the search
                     * after Triangle above. */
                    resync_input(&previous);
                } else {
                    ch->cursor = row;
                }
            }
            /* Cross with the selection carried off screen by a drag or a
             * fling picks a row on screen, as a direction does (see
             * cursor_into_view), instead of opening one the user cannot
             * see. */
            if ((pressed & SCE_CTRL_CROSS) &&
                !cursor_into_view(&ch->scroll, &ch->cursor, ch->count,
                                  CHANNEL_ROW_H)) {
                run_channel_detail(&ch->items[ch->cursor]);
                resync_input(&previous);
            }
            /* run_channel_detail() may have just switched this tab to
             * VIEW_CHANNEL_DETAIL — recompute so the draw pass below shows
             * the new screen instead of one stale frame of the old list. */
            channels = active_channel_list();
            videos = active_video_list();
        } else if (!channels && g_tab == TAB_HOME &&
                   g_view[TAB_HOME] == VIEW_VIDEOS && g_home.count == 0 &&
                   (pressed & SCE_CTRL_CROSS)) {
            /* The empty Start screen: Cross retries the fetch right here
             * instead of asking for an app restart. */
            run_home();
            resync_input(&previous);
        } else if (!channels && videos && videos->count > 0) {
            if (direction &&
                cursor_into_view(&videos->scroll, &videos->cursor,
                                 videos->count, ROW_H))
                direction = 0;
            if (direction & SCE_CTRL_UP && videos->cursor > 0) videos->cursor--;
            if (direction & SCE_CTRL_DOWN) {
                if (videos->cursor < videos->count - 1) {
                    videos->cursor++;
                } else if (videos == &g_search && videos->continuation[0]) {
                    load_more_search();
                    resync_input(&previous);
                    if (videos->cursor < videos->count - 1) videos->cursor++;
                }
            }
            if (direction & SCE_CTRL_RIGHT) {
                videos->cursor += VISIBLE_ROWS;
                if (videos->cursor > videos->count - 1)
                    videos->cursor = videos->count - 1;
            }
            if (direction & SCE_CTRL_LEFT) {
                videos->cursor -= VISIBLE_ROWS;
                if (videos->cursor < 0) videos->cursor = 0;
            }

            /* This used to be where touch scrolling happened: on release, a
             * drag of more than 36 px either way moved `top` by three whole
             * rows, so nothing followed the finger and every swipe ended in
             * the same jump. All of that is list_scroll_step() now. */
            unsigned settled;
            int row = list_scroll_step(&videos->scroll, &videos->cursor,
                                       &videos->top, videos->count, ROW_H,
                                       touch_flags, &touch, &settled);
            if (row >= 0) {
                if (row == videos->cursor) {
                    player_screen_run(&videos->items[row]);
                    sceCtrlPeekBufferPositive(0, &previous, 1);
                    ui_touch_reset();
                } else {
                    videos->cursor = row;
                }
            } else if ((settled & VT_KINETIC_SETTLED_AT_END) &&
                       videos == &g_search && videos->continuation[0]) {
                /* The touch side of the D-pad's "Down on the last row loads
                 * the next page": flinging into the end of the results, or
                 * pulling up past it, asks for more. Only once the list is
                 * at rest, so the spinner does not cut the bounce off. */
                load_more_search();
                resync_input(&previous);
            }
            /* Same as in the channel list: no playing a row that a drag or
             * a fling has carried off screen. */
            if ((pressed & SCE_CTRL_CROSS) &&
                !cursor_into_view(&videos->scroll, &videos->cursor,
                                  videos->count, ROW_H)) {
                player_screen_run(&videos->items[videos->cursor]);
                sceCtrlPeekBufferPositive(0, &previous, 1);
                ui_touch_reset();
            }
        }

        vt_thumbs_pump();

        vita2d_start_drawing();
        vita2d_clear_screen();
        const char *TAB_LABELS[TAB_COUNT];
        int tab_badges[TAB_COUNT];
        tab_bar_content(TAB_LABELS, tab_badges);
        char subtitle[300] = {0};
        /* Most specific view first: the tab-level TAB_SEARCH default below
         * would otherwise win over the recent-searches header once any
         * search has run, since g_search_query stays set. */
        if (g_view[g_tab] == VIEW_CHANNEL_DETAIL)
            snprintf(subtitle, sizeof subtitle, "%s", g_channel.title);
        else if (g_tab == TAB_SEARCH && g_view[TAB_SEARCH] == VIEW_SEARCHES)
            snprintf(subtitle, sizeof subtitle, "%s",
                     vt_str(S_RECENT_SEARCHES));
        else if (g_tab == TAB_SEARCH && g_search_query[0])
            snprintf(subtitle, sizeof subtitle, "%s%s", g_search_query,
                     g_view[TAB_SEARCH] == VIEW_CHANNELS
                         ? vt_str(S_SUFFIX_CHANNELS) : "");
        else if (g_tab == TAB_ABOS && g_view[TAB_ABOS] == VIEW_VIDEOS)
            snprintf(subtitle, sizeof subtitle, "%s", vt_str(S_NEW_VIDEOS));
        else if (g_tab == TAB_HOME && g_view[TAB_HOME] == VIEW_HISTORY)
            snprintf(subtitle, sizeof subtitle, "%s",
                     vt_str(S_RECENTLY_WATCHED));
        vt_ui_top_bar(subtitle, yt_oauth_logged_in());
        vt_ui_tab_bar_badged(TAB_LABELS, tab_badges, TAB_COUNT, g_tab);

        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(0, LIST_Y, VT_SCREEN_W, VT_SCREEN_H - HINT_H);
        if (g_tab == TAB_DL) {
            downloads_screen_draw(LIST_Y, LIST_BOTTOM);
        } else if (g_tab == TAB_MORE) {
            settings_screen_draw(LIST_Y);
        } else if (channels) {
            if (channels->count == 0) {
                const char *msg = vt_str(
                    g_tab == TAB_SEARCH ? S_NO_CHANNELS_FOUND
                    : yt_oauth_logged_in() ? S_NO_SUBSCRIBED_CHANNELS
                                           : S_SIGN_IN_MORE_TAB);
                int w = vt_ui_text_width(1.0f, msg);
                vt_ui_text((VT_SCREEN_W - w) / 2, LIST_Y + 100, VT_COLOR_MUTED,
                          1.0f, msg);
            } else {
                int off = vt_kinetic_draw_offset(&channels->scroll.kinetic);
                for (int i = off > 0 ? off / CHANNEL_ROW_H : 0;
                     i < channels->count; i++) {
                    int y = LIST_Y + i * CHANNEL_ROW_H - off;
                    if (y >= LIST_BOTTOM) break;
                    draw_channel_row(&channels->items[i], y,
                                     i == channels->cursor, LIST_Y,
                                     LIST_BOTTOM);
                }
            }
        } else if (showing_recent_searches()) {
            int n = vt_store_search_count();
            if (n == 0) {
                /* On the Suche tab this is still the first-run instruction;
                 * on Start it is a list that has nothing in it yet. */
                const char *empty = vt_str(
                    g_view[TAB_SEARCH] == VIEW_SEARCHES ? S_NOTHING_SEARCHED_YET
                                                        : S_SEARCH_EMPTY);
                int w = vt_ui_text_width(1.1f, empty);
                vt_ui_text((VT_SCREEN_W - w) / 2, LIST_Y + 100, VT_COLOR_MUTED,
                          1.1f, empty);
            } else {
                vt_ui_text(24, LIST_Y + 12, VT_COLOR_FAINT, 0.85f,
                           vt_str(S_RECENT_SEARCHES));
                for (int i = 0; i < n; i++) {
                    int y = LIST_Y + 44 + i * 40;
                    if (i == g_recent_cursor) {
                        vt_ui_rect(8, y - 6, VT_SCREEN_W - 16, 38,
                                   VT_COLOR_FOCUS);
                        vt_ui_rect(8, y - 6, 4, 38, VT_COLOR_ACCENT);
                    }
                    vt_ui_text_clipped(28, y, VT_COLOR_TEXT, 1.0f,
                                       VT_SCREEN_W - 56,
                                       vt_store_search_at(i));
                }
            }
        } else if (g_view[g_tab] == VIEW_HISTORY && g_history.count == 0) {
            const char *empty = vt_str(S_NOTHING_WATCHED_YET);
            int w = vt_ui_text_width(1.1f, empty);
            vt_ui_text((VT_SCREEN_W - w) / 2, LIST_Y + 100, VT_COLOR_MUTED,
                      1.1f, empty);
        } else if (g_tab == TAB_HOME && (!videos || videos->count == 0)) {
            draw_empty_home(LIST_Y, yt_oauth_logged_in());
        } else if (videos && videos->count == 0) {
            const char *empty = vt_str(g_tab == TAB_SEARCH ? S_SEARCH_EMPTY
                                                           : S_NO_VIDEOS);
            int w = vt_ui_text_width(1.1f, empty);
            vt_ui_text((VT_SCREEN_W - w) / 2, LIST_Y + 100, VT_COLOR_MUTED,
                      1.1f, empty);
        } else if (videos) {
            /* Every row that overlaps the band, at the same whole-pixel
             * offset the touch code hit-tests against. */
            int off = vt_kinetic_draw_offset(&videos->scroll.kinetic);
            for (int i = off > 0 ? off / ROW_H : 0; i < videos->count; i++) {
                int y = LIST_Y + i * ROW_H - off;
                if (y >= LIST_BOTTOM) break;
                draw_video_row(&videos->items[i], y, i == videos->cursor,
                               LIST_Y, LIST_BOTTOM);
            }
        }
        vita2d_disable_clipping();
        downloads_snackbar_draw(LIST_BOTTOM);
        /* Over everything above, so the dimming also covers the bars; the
         * hint bar below is drawn after it and stays readable. */
        downloads_menu_draw();

        /* Rows to ask about: the hints of the video lists name SELECT as the
         * download menu only when there is a row for it to open on. */
        int rows_focus = videos && !channels && videos->count > 0;
        char hint_buf[160];
        const char *hint;
        if (downloads_menu_active())
            hint = vt_str(S_HINT_SETTINGS_MODAL);
        else if (g_tab == TAB_DL)
            hint = downloads_screen_hint(hint_buf, sizeof hint_buf);
        else if (g_tab == TAB_MORE)
            hint = settings_screen_hint();
        else if (g_view[g_tab] == VIEW_HISTORY)
            hint = vt_str(rows_focus ? S_HINT_HISTORY_DL : S_HINT_HISTORY);
        else if (g_view[g_tab] == VIEW_SEARCHES)
            hint = vt_str(vt_store_search_count() > 0 ? S_HINT_SEARCHES_VIEW
                                                     : S_HINT_SEARCHES_EMPTY);
        /* The detail view before the tab-level search cases: a search with
         * channel hits but no videos leaves g_search.count at 0, and the
         * channel opened from it would otherwise get the "search again"
         * legend while Cross plays and Square subscribes. */
        else if (g_view[g_tab] == VIEW_CHANNEL_DETAIL) {
            int subscribed = subscription_index(g_channel.channel_id) >= 0;
            hint = vt_str(rows_focus
                              ? (subscribed ? S_HINT_CHANNEL_UNSUB_DL
                                            : S_HINT_CHANNEL_SUB_DL)
                              : (subscribed ? S_HINT_CHANNEL_UNSUB
                                            : S_HINT_CHANNEL_SUB));
        }
        else if (g_tab == TAB_SEARCH && g_view[TAB_SEARCH] == VIEW_VIDEOS &&
                 !channels && g_search.count == 0 &&
                 vt_store_search_count() > 0)
            hint = vt_str(S_HINT_SEARCH_RECENT);
        else if (g_tab == TAB_SEARCH && channels)
            hint = vt_str(S_HINT_SEARCH_CHANNELS);
        else if (g_tab == TAB_SEARCH) {
            snprintf(hint_buf, sizeof hint_buf,
                     vt_str(rows_focus ? S_HINT_SEARCH_VIDEOS_DL
                                       : S_HINT_SEARCH_VIDEOS),
                     g_search_channels.count);
            hint = hint_buf;
        } else if (channels)
            hint = vt_str(channels_sort() == CH_SORT_RECENT
                              ? S_HINT_SUBS_CHANNELS_RECENT
                              : S_HINT_SUBS_CHANNELS_ALPHA);
        else if (g_tab == TAB_ABOS)
            hint = vt_str(rows_focus ? S_HINT_SUBS_VIDEOS_DL
                                     : S_HINT_SUBS_VIDEOS);
        /* TAB_HOME falls through to the generic hint below. */
        else
            hint = vt_str(rows_focus ? S_HINT_HOME_DL : S_HINT_HOME);
        vt_ui_hint_bar(hint);

        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }
}
