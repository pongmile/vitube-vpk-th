/* ViTube — offline downloads: the one header the engine, the Downloads screen
 * and the player share.
 *
 * What is downloaded: a video's progressive MP4 (the same muxed 360p-class
 * format the player's direct path streams — yt_resolve_stream's formats[]).
 * 720p exists only as HLS segments and is out of scope here. Files live in
 * VITUBE_DL_DIR:
 *
 *   <id>.part     the bytes received so far; the only file that ever grows
 *   <id>.mp4      the finished, verified video (renamed from .part)
 *   <id>.jpg      the thumbnail, best effort (see dl_thumb_path)
 *   index.json    what is queued/paused/done, rewritten atomically
 *
 * <id> is an 11 character YouTube id and nothing else: dl_enqueue rejects
 * anything outside [A-Za-z0-9_-], so no title or id can ever name a path.
 *
 * Threading contract. Everything here may be called from the UI thread, in
 * the middle of a frame, so no call blocks on the network or does bulk I/O
 * (the worst case is a few milliseconds behind the file lock — see log.h:
 * every ux0: access in this app goes through vt_io_lock, a hardware crash
 * once traced back to concurrent SceIo). The engine runs on its own worker
 * thread; snapshots are copies, never pointers into engine state. */
#ifndef VITUBE_DL_DL_H
#define VITUBE_DL_DL_H

#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "yt/model.h"

#define VITUBE_DL_DIR VITUBE_DATA_DIR "/downloads"
#define VITUBE_DL_INDEX_PATH VITUBE_DL_DIR "/index.json"

/* Free space that is never used up: state.json, the log and the thumbnail
 * cache live on the same card, and a Vita whose ux0: is completely full
 * misbehaves. A download starts only if its size + this fits in what is
 * free; a write that still fails with "no space" ends it as DL_ERR_DISK_FULL
 * (and keeps the .part, so freeing space and resuming loses nothing). */
#define DL_RESERVE_BYTES (128LL * 1024 * 1024)

typedef enum {
    DL_QUEUED,       /* waiting for its turn: one download runs at a time */
    DL_RESOLVING,    /* asking YouTube for the stream URL (first start, or the
                      * URL expired — googlevideo URLs live about six hours) */
    DL_DOWNLOADING,
    DL_PAUSED,       /* the user paused it; stays paused across app restarts */
    DL_WAITING,      /* Wi-Fi lost or an online video is playing; continues by
                      * itself (retry_in_s says when it tries again) */
    DL_VERIFYING,    /* every byte is here; checking the file is sound */
    DL_FAILED,       /* see DlItem.error; dl_resume() retries it */
    DL_DONE          /* complete and verified: playable offline */
} DlState;

typedef enum {
    DL_ERR_NONE,
    DL_ERR_NO_SPACE,        /* size + DL_RESERVE_BYTES does not fit in free space */
    DL_ERR_DISK_FULL,       /* the card filled up while writing */
    DL_ERR_NOT_DOWNLOADABLE,/* live stream, no progressive format, age/region
                             * gate: nothing to save */
    DL_ERR_VERIFY,          /* the finished file is not a sound MP4; the .part
                             * is dropped and dl_resume() starts over */
    DL_ERR_IO,              /* any other file error */
    DL_ERR_NETWORK,         /* connected, yet repeated attempts all failed */
    DL_ERR_URL              /* the stream URL could not be renewed */
} DlError;

/* One row of the Downloads screen. A plain copy. */
typedef struct {
    char video_id[YT_VIDEO_ID_LEN];
    char title[YT_TITLE_MAX];
    char channel[YT_CHANNEL_MAX];
    char duration[YT_META_MAX];   /* "12:34", as the list row showed it */
    DlState state;
    DlError error;                /* meaningful when state == DL_FAILED, and
                                   * DL_ERR_NO_SPACE also keeps bytes_total so
                                   * the UI can say how much was needed */
    int height;                   /* of the format being saved; 0 = unknown */
    int64_t bytes_done;           /* contiguous bytes safely on the card */
    int64_t bytes_total;          /* 0 until the server has said */
    int bytes_per_sec;            /* smoothed; 0 unless DOWNLOADING */
    int retry_in_s;               /* DL_WAITING only: seconds to the next
                                   * attempt, -1 when it waits for an event
                                   * (Wi-Fi back, playback over) */
} DlItem;

typedef struct {
    int64_t total_bytes;          /* ux0: capacity */
    int64_t free_bytes;           /* ux0: free right now */
    int64_t downloads_bytes;      /* what the downloads occupy (done + .part) */
    int low;                      /* free_bytes < DL_RESERVE_BYTES: starting a
                                   * download would be refused */
} DlStorage;

typedef enum {
    DL_ENQ_OK,       /* queued */
    DL_ENQ_EXISTS,   /* already queued, running, paused or done: nothing to do */
    DL_ENQ_FULL,     /* free space is already below DL_RESERVE_BYTES */
    DL_ENQ_BAD       /* not a valid video id, or the engine is not running */
} DlEnqueue;

/* --- life cycle ----------------------------------------------------------
 * dl_init after http_init() and vt_netwatch_start(): it creates the
 * directory, reads index.json, reconciles every entry with the files on the
 * card (the .part's size is the truth about bytes_done, not the index — the
 * index is only rewritten every few seconds) and starts the worker. Entries
 * that were running when the app last closed become DL_QUEUED and continue;
 * DL_PAUSED ones stay paused. A DL_DONE entry whose file is gone or has the
 * wrong size is dropped from the list. Returns 0, or <0 when the engine
 * cannot run (downloads are then simply unavailable; the app goes on).
 * dl_shutdown stops the worker, flushes the index and joins; before
 * http_shutdown(). */
int dl_init(void);
void dl_shutdown(void);

/* --- commands (non-blocking) ---------------------------------------------- */
DlEnqueue dl_enqueue(const yt_video_t *video);
void dl_pause(const char *video_id);
/* Continues a paused item, retries a failed one (a DL_ERR_VERIFY one from the
 * first byte), and gives a waiting one an immediate attempt. */
void dl_resume(const char *video_id);
void dl_pause_all(void);
void dl_resume_all(void);
/* Stops the item if it is running and removes it from the list together with
 * its .part/.mp4/.jpg files. Works in every state, including DL_DONE. */
void dl_delete(const char *video_id);

/* --- queries (non-blocking) ----------------------------------------------- */
int dl_count(void);
/* Copies up to `max` items, oldest first, and returns how many. */
int dl_snapshot(DlItem *out, int max);
/* 1 and fills *out when the id is on the list, else 0. out may be NULL. */
int dl_get(const char *video_id, DlItem *out);
/* Number of items that are not finished and not stopped: QUEUED, RESOLVING,
 * DOWNLOADING, WAITING, VERIFYING. For a badge on the tab. */
int dl_active_count(void);
/* Bumped on every change a screen could show (state, progress step, list
 * change), so a frame loop can compare it against its last copy instead of
 * diffing snapshots. Progress changes bump it at most a few times a second. */
unsigned dl_revision(void);
/* Free/total/used on ux0: — what "storage full?" is answered from. <0 when
 * the card cannot be queried (then the other fields are 0, low is 0). */
int dl_storage(DlStorage *out);

/* --- playing what was downloaded -------------------------------------------
 * 1 and the path of the finished, verified .mp4 when the video is DL_DONE and
 * the file is still there; else 0. The player prefers it to streaming. */
int dl_local_path(const char *video_id, char *path, size_t cap);
/* 1 and the path of the saved thumbnail (a JPEG) if one exists, else 0. */
int dl_thumb_path(const char *video_id, char *path, size_t cap);
/* The player could not open or decode a file dl_local_path gave it: the entry
 * becomes DL_FAILED/DL_ERR_VERIFY, ready for dl_resume() to fetch it again. */
void dl_report_bad_file(const char *video_id);

/* --- sharing the connection with playback ----------------------------------
 * Bracket the part of an ONLINE playback that uses the network (not an
 * offline one): the engine holds its transfer (items show DL_WAITING with
 * retry_in_s -1) so a 720p stream is not starved, and carries on after
 * dl_playback_end. Calls nest. */
void dl_playback_begin(void);
void dl_playback_end(void);

/* Stable lower-case name of an error for the log, e.g. "no_space". Never shown
 * to the user: the UI words each error in the user's own language. */
const char *dl_error_key(DlError error);

#endif
