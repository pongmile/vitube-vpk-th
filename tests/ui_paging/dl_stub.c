/* Throwaway in-memory stand-in for the download engine, for the UI harness. */
#include "dl/dl.h"

#include <stdio.h>
#include <string.h>

#include "stub_ctl.h"

#define CAP 512
static DlItem g_items[CAP];
static int g_n;
static unsigned g_rev = 1;
DlStorage stub_storage = { 31999000000LL, 12400000000LL, 1200000000LL, 0 };
int stub_storage_rc = 0;
DlEnqueue stub_enqueue_result = DL_ENQ_OK;
char stub_calls[64][96];
int stub_ncalls;
int stub_thumb_ids; /* bit per last char of id: 1 = has thumb */
int stub_snapshot_calls, stub_storage_calls, stub_thumbpath_calls;

static void log_call(const char *fmt, const char *id) {
    if (stub_ncalls < 64) snprintf(stub_calls[stub_ncalls++], 96, fmt, id ? id : "");
}

void stub_dl_clear(void) { g_n = 0; g_rev++; stub_ncalls = 0; }
DlItem *stub_dl_add(const char *id, const char *title, const char *channel,
                    const char *duration, DlState state) {
    if (g_n >= CAP) return &g_items[CAP - 1];
    DlItem *it = &g_items[g_n++];
    memset(it, 0, sizeof *it);
    snprintf(it->video_id, sizeof it->video_id, "%s", id);
    snprintf(it->title, sizeof it->title, "%s", title);
    snprintf(it->channel, sizeof it->channel, "%s", channel);
    snprintf(it->duration, sizeof it->duration, "%s", duration);
    it->state = state;
    g_rev++;
    return it;
}
void stub_dl_touch(void) { g_rev++; }
void stub_dl_remove(const char *id) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_items[i].video_id, id)) {
            memmove(&g_items[i], &g_items[i + 1], (size_t)(g_n - i - 1) * sizeof *g_items);
            g_n--;
            g_rev++;
            return;
        }
}
DlItem *stub_dl_find(const char *id) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_items[i].video_id, id)) return &g_items[i];
    return NULL;
}

int dl_init(void) { return 0; }
void dl_shutdown(void) {}
DlEnqueue dl_enqueue(const yt_video_t *v) {
    log_call("enqueue:%s", v->video_id);
    if (stub_enqueue_result == DL_ENQ_OK) {
        stub_dl_add(v->video_id, v->title, v->channel, v->duration, DL_QUEUED);
    }
    return stub_enqueue_result;
}
void dl_pause(const char *id) {
    log_call("pause:%s", id);
    DlItem *it = stub_dl_find(id);
    if (it) { it->state = DL_PAUSED; it->bytes_per_sec = 0; g_rev++; }
}
void dl_resume(const char *id) {
    log_call("resume:%s", id);
    DlItem *it = stub_dl_find(id);
    if (it) { it->state = DL_QUEUED; it->error = DL_ERR_NONE; g_rev++; }
}
void dl_pause_all(void) { log_call("pause_all%s", NULL); }
void dl_resume_all(void) { log_call("resume_all%s", NULL); }
void dl_delete(const char *id) { log_call("delete:%s", id); stub_dl_remove(id); }
int dl_count(void) { return g_n; }
int dl_snapshot(DlItem *out, int max) {
    stub_snapshot_calls++;
    int n = g_n < max ? g_n : max;
    memcpy(out, g_items, (size_t)n * sizeof *out);
    return n;
}
int dl_get(const char *id, DlItem *out) {
    DlItem *it = stub_dl_find(id);
    if (!it) return 0;
    if (out) *out = *it;
    return 1;
}
int dl_active_count(void) {
    int n = 0;
    for (int i = 0; i < g_n; i++) {
        DlState s = g_items[i].state;
        if (s == DL_QUEUED || s == DL_RESOLVING || s == DL_DOWNLOADING || s == DL_WAITING || s == DL_VERIFYING) n++;
    }
    return n;
}
unsigned dl_revision(void) { return g_rev; }
int dl_storage(DlStorage *out) {
    stub_storage_calls++;
    if (stub_storage_rc < 0) { memset(out, 0, sizeof *out); return -1; }
    *out = stub_storage;
    return 0;
}
int dl_local_path(const char *id, char *path, size_t cap) { (void)id; (void)path; (void)cap; return 0; }
int dl_thumb_path(const char *id, char *path, size_t cap) {
    stub_thumbpath_calls++;
    size_t n = strlen(id);
    if (!(stub_thumb_ids & (1 << (id[n - 1] & 7)))) return 0;
    snprintf(path, cap, "ux0:data/ViTube/downloads/%s.jpg", id);
    return 1;
}
void dl_report_bad_file(const char *id) { (void)id; }
void dl_playback_begin(void) {}
void dl_playback_end(void) {}
const char *dl_error_key(DlError e) { (void)e; return "x"; }
