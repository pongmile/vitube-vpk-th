#pragma once
#include "dl/dl.h"
#include "ui/touch.h"
void stub_dl_clear(void);
DlItem *stub_dl_add(const char *id, const char *title, const char *channel, const char *duration, DlState state);
void stub_dl_touch(void);
void stub_dl_remove(const char *id);
DlItem *stub_dl_find(const char *id);
extern DlStorage stub_storage;
extern int stub_storage_rc;
extern DlEnqueue stub_enqueue_result;
extern char stub_calls[64][96];
extern int stub_ncalls;
extern int stub_thumb_ids;
extern int stub_snapshot_calls, stub_storage_calls, stub_thumbpath_calls;

/* harness control, h_stubs.c */
extern int g_frame;
extern unsigned g_pad_buttons;
extern int g_connected;
extern char g_played[16][16];
extern int g_nplayed;
extern int h_touch_resets;
extern int h_thumb_net_calls, h_thumb_file_calls;
extern int h_home_count;
extern void (*h_in_player)(void);
void h_set_touch(unsigned flags, int x, int y, int dx, int dy);
int h_text_seen(const char *needle);
/* Counts how often a text comes up on screen. h_watch() starts the count (and
 * zeroes it); from then on every frame that is swapped in is looked at:
 * h_watch_appearances is how many times the text went from absent to present
 * (one toast = one appearance), h_watch_frames how many frames it was there.
 * A toast is modal, so a scenario cannot sample it often enough from outside. */
void h_watch(const char *needle);
extern int h_watch_appearances, h_watch_frames;
int h_op_count(char kind);
unsigned h_text_color(const char *needle);
int h_text_y(const char *needle);
int h_prev_disables(void);
void h_dump(const char *name);
typedef int (*ScriptFn)(int frame);
extern ScriptFn h_script;
