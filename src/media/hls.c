/* ViTube — HLS playlist handling for the decoder. See hls.h. */
#include "media/hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita_https.h>

#include "log.h"

#define HLS_USER_AGENT \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 " \
    "(KHTML, like Gecko) Version/26.0 Safari/605.1.15"

/* A single 720p segment is a few hundred KB; the playlists themselves run to
 * ~550 KB because every one of a few hundred segment URLs is ~1.2 KB long. */
#define HLS_MAX_FETCH_BYTES (16 * 1024 * 1024)

/* ---------------------------------------------------------------- parsing */

/* Value of `name` inside an HLS attribute list, honouring quoted values
 * (CODECS="avc1.4D401F,mp4a.40.2" contains a comma, so a plain comma split
 * would tear it apart). Returns the length written, 0 when absent. */
static size_t attribute(const char *line, const char *name, char *out,
                        size_t cap) {
    if (!line || !name || !out || !cap) return 0;
    size_t name_len = strlen(name);
    for (const char *p = line; *p && *p != '\n'; p++) {
        int at_start = (p == line) || p[-1] == ',' || p[-1] == ':';
        if (!at_start || strncmp(p, name, name_len) != 0 || p[name_len] != '=')
            continue;
        const char *value = p + name_len + 1;
        int quoted = *value == '"';
        if (quoted) value++;
        const char *end = value;
        while (*end && *end != '\n' && !(quoted ? *end == '"' : *end == ','))
            end++;
        size_t len = (size_t)(end - value);
        if (len >= cap) len = cap - 1;
        memcpy(out, value, len);
        out[len] = '\0';
        return len;
    }
    if (cap) out[0] = '\0';
    return 0;
}

/* Copies the line at `p` (without its newline) into out. */
static void copy_line(const char *p, char *out, size_t cap) {
    if (!p || !out || !cap) return;
    size_t len = 0;
    while (p[len] && p[len] != '\n' && p[len] != '\r' && len < cap - 1) len++;
    memcpy(out, p, len);
    out[len] = '\0';
}

static const char *next_line(const char *p) {
    const char *nl = strchr(p, '\n');
    return nl ? nl + 1 : NULL;
}

/* Whether a CODECS list names an H.264 track. The entries come in no fixed
 * order — YouTube's on-demand manifest writes CODECS="avc1…,mp4a…" and its
 * live one CODECS="mp4a…,avc1…" — so every entry is looked at rather than
 * just the start of the string. Matching the start alone left the live
 * manifest looking like it offered no playable variant at all. */
static int codecs_have_h264(const char *codecs) {
    for (const char *p = codecs; *p;) {
        if (strncmp(p, "avc1", 4) == 0) return 1;
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
        while (*p == ' ') p++;
    }
    return 0;
}

int vt_hls_pick(const char *master, int max_height, int max_fps,
                char *video_url, size_t video_cap,
                char *audio_url, size_t audio_cap) {
    if (!master || !video_url || !audio_url) return -1;
    video_url[0] = audio_url[0] = '\0';

    char best_group[64] = {0};
    int best_height = 0;
    int best_smooth = 0;
    long best_bandwidth = 0;
    int best_fps = 0;

    for (const char *p = master; p; p = next_line(p)) {
        if (strncmp(p, "#EXT-X-STREAM-INF:", 18) != 0) continue;
        char codecs[128], resolution[32], group[64], bandwidth[32], rate[32];
        attribute(p, "CODECS", codecs, sizeof codecs);
        attribute(p, "RESOLUTION", resolution, sizeof resolution);
        attribute(p, "AUDIO", group, sizeof group);
        attribute(p, "BANDWIDTH", bandwidth, sizeof bandwidth);
        attribute(p, "FRAME-RATE", rate, sizeof rate);
        /* Only H.264: the Vita's hardware decoder does nothing else, and the
         * master playlist also advertises VP9 and AV1 variants. */
        if (!codecs_have_h264(codecs)) continue;
        int width = 0, height = 0;
        if (sscanf(resolution, "%dx%d", &width, &height) != 2) continue;
        if (height > max_height) continue;
        long bw = strtol(bandwidth, NULL, 10);
        int fps = (int)(strtod(rate, NULL) + 0.5);
        /* A variant this decoder can actually keep up with outranks a bigger
         * one it cannot: resolution is worth nothing if half the frames get
         * dropped to stay near the audio clock. Unstated frame rates count as
         * fine — only an explicit, explicitly too-high one is a demotion. */
        int smooth = max_fps <= 0 || fps <= 0 || fps <= max_fps;
        if (video_url[0]) {
            if (smooth < best_smooth) continue;
            if (smooth == best_smooth) {
                if (height < best_height) continue;
                if (height == best_height && bw <= best_bandwidth) continue;
            }
        }
        const char *uri = next_line(p);
        while (uri && (*uri == '#' || *uri == '\n' || *uri == '\r'))
            uri = next_line(uri);
        if (!uri || !*uri) continue;
        best_height = height;
        best_bandwidth = bw;
        best_smooth = smooth;
        best_fps = fps;
        snprintf(best_group, sizeof best_group, "%s", group);
        copy_line(uri, video_url, video_cap);
    }

    if (!video_url[0]) {
        vt_log("hls: no H.264 variant at or below %dp", max_height);
        return -1;
    }
    if (max_fps > 0 && !best_smooth)
        vt_log("hls: nothing at or below %d fps; taking %dp%d anyway", max_fps,
               best_height, best_fps);

    /* A variant that names no AUDIO group is muxed: its segments carry both
     * streams, so there is no second playlist to look for. That is how
     * YouTube serves live, and the caller answers it by opening the one
     * playlist with both decoder cursors instead of one each. */
    if (!best_group[0]) {
        vt_log("hls: picked %dp%d (%ld bps), muxed — one playlist for both",
               best_height, best_fps, best_bandwidth);
        return 0;
    }

    /* A dubbed video carries one EXT-X-MEDIA entry PER LANGUAGE in the same
     * group, and taking the first match handed such videos whichever dub
     * YouTube listed first (observed 2026-08-29: German video with an
     * auto-dub — English audio after the HD switch, because the en-US
     * rendition came first and BOTH renditions said DEFAULT=NO). The direct
     * 360p format the app starts on always plays the ORIGINAL track, and
     * YouTube marks that rendition with the stable "- original" suffix in
     * NAME ("Deutsch (Deutschland) - original" vs "American English -
     * dubbed-auto") — so preferring it keeps the language from changing
     * mid-video. Single-audio videos name their one rendition "Default"
     * with DEFAULT=YES, hence the fallback chain: original, then
     * DEFAULT=YES, then the first rendition of the group. */
    char chosen_name[64] = {0};
    int chosen_score = 0;
    for (const char *p = master; p; p = next_line(p)) {
        if (strncmp(p, "#EXT-X-MEDIA:", 13) != 0) continue;
        char type[32], group[64], is_default[16], name[64];
        attribute(p, "TYPE", type, sizeof type);
        attribute(p, "GROUP-ID", group, sizeof group);
        if (strcmp(type, "AUDIO") != 0 || strcmp(group, best_group) != 0)
            continue;
        attribute(p, "DEFAULT", is_default, sizeof is_default);
        attribute(p, "NAME", name, sizeof name);
        int score = 1;
        if (strcmp(is_default, "YES") == 0) score = 2;
        if (strstr(name, "original")) score = 3;
        if (!audio_url[0] || score > chosen_score) {
            attribute(p, "URI", audio_url, audio_cap);
            snprintf(chosen_name, sizeof chosen_name, "%s", name);
            chosen_score = score;
        }
        if (chosen_score == 3) break;
    }

    if (!audio_url[0]) {
        vt_log("hls: variant %dp has no audio rendition for group %s",
               best_height, best_group);
        return -1;
    }
    vt_log("hls: picked %dp%d (%ld bps), audio group %s track \"%s\" (%s)",
           best_height, best_fps, best_bandwidth, best_group, chosen_name,
           chosen_score == 3 ? "original" : chosen_score == 2 ? "default"
                                                              : "first");
    return 0;
}

/* Rewrites every playlist URI from https: to file: in place (the result is
 * always shorter). libavformat's hls demuxer looks the scheme up with
 * avio_find_protocol_name() and bails out before io_open is ever consulted,
 * and this FFmpeg build registers no protocol but "file" — so the scheme has
 * to be one it knows, and the fetch callback maps it back. Only URI
 * positions are touched: a bare line, or the value of URI="…". A query
 * string that happens to contain "https://" is left alone. */
static void rewrite_uris(char *playlist) {
    static const char https[] = "https://";
    static const char file[] = "file://";
    char *read = playlist, *write = playlist;
    int at_line_start = 1;
    while (*read) {
        int is_uri_start = at_line_start;
        /* Looking BEHIND the write pointer, not the read pointer: the rewrite
         * compacts in place (file: is one byte shorter than https:), so once
         * the first URI has been shortened the bytes before `read` are the
         * shifted remains of later input, while the output always holds the
         * verbatim preceding text. */
        if (!is_uri_start && write >= playlist + 5 &&
            strncmp(write - 5, "URI=\"", 5) == 0)
            is_uri_start = 1;
        if (is_uri_start && strncmp(read, https, sizeof https - 1) == 0) {
            memcpy(write, file, sizeof file - 1);
            write += sizeof file - 1;
            read += sizeof https - 1;
            at_line_start = 0;
            continue;
        }
        at_line_start = (*read == '\n');
        *write++ = *read++;
    }
    *write = '\0';
}

/* ---------------------------------------------------------------- fetching */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} fetch_buf_t;

static size_t fetch_write(const void *data, size_t size, void *opaque) {
    fetch_buf_t *buf = opaque;
    if (buf->len + size + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 65536;
        while (cap < buf->len + size + 1) cap *= 2;
        if (cap > HLS_MAX_FETCH_BYTES) return 0; /* aborts the transfer */
        char *grown = realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    buf->data[buf->len] = '\0';
    return size;
}

/* One plain GET. The global http.c client is deliberately not used: these
 * calls come from the decoder's cursor threads, and vita_https clients are
 * treated as single-owner everywhere in this app. */
static int fetch_into(struct VitaHttpsClient *client, const char *url,
                      volatile int *cancel_flag, fetch_buf_t *out) {
    VitaHttpsRequest request = {
        .method = NULL, /* GET */
        .url = url,
        .write = fetch_write,
        .write_opaque = out,
        .cancel_flag = cancel_flag,
    };
    VitaHttpsResponse response = {0};
    int ret = vita_https_perform(client, &request, &response);
    if (ret < 0) {
        /* A 4xx/5xx arrives as ERROR_HTTP_STATUS with the code filled in;
         * unlike the API layer this is never recoverable here, a segment
         * either comes through or playback cannot continue. */
        vt_log("hls: fetch failed ret=%d status=%ld", ret,
               response.status_code);
        return ret;
    }
    if (response.status_code && response.status_code != 200 &&
        response.status_code != 206) {
        vt_log("hls: fetch HTTP %ld", response.status_code);
        return -1;
    }
    return 0;
}

int vt_hls_fetch_master(const char *url, volatile int *cancel_flag,
                        char **text_out) {
    if (!url || !text_out) return -1;
    *text_out = NULL;
    VitaHttpsClientConfig config = {
        .user_agent = HLS_USER_AGENT,
        .connect_timeout_ms = 10000,
        .request_timeout_ms = 20000,
    };
    struct VitaHttpsClient *client = vita_https_client_create(&config);
    if (!client) return -1;
    fetch_buf_t buf = {0};
    int ret = fetch_into(client, url, cancel_flag, &buf);
    vita_https_client_destroy(client);
    if (ret < 0 || !buf.len) {
        free(buf.data);
        return ret < 0 ? ret : -1;
    }
    *text_out = buf.data;
    return 0;
}

/* ---------------------------------------------------------------- playlist */

/* One fetched playlist, kept alive by everyone still reading it.
 *
 * A live playlist is replaced while playback runs, and the cursors read the
 * bytes of the one they were opened on — the audio cursor in particular opens
 * seconds after the video one, by which time a reload may already be in
 * flight. Refcounting is what lets the worker install a new snapshot without
 * ever freeing text somebody is still parsing. Refs are taken and dropped
 * under worker_lock. */
typedef struct VtHlsPlaylist {
    int refs;
    char *text;      /* URI-rewritten, NUL-terminated; owned */
    size_t len;
} hls_playlist_t;

/* Adopts `text` on success (and frees it on failure). */
static hls_playlist_t *playlist_new(char *text) {
    hls_playlist_t *playlist = calloc(1, sizeof *playlist);
    if (!playlist) {
        free(text);
        return NULL;
    }
    playlist->refs = 1;
    playlist->text = text;
    playlist->len = strlen(text);
    return playlist;
}

static void playlist_retain(hls_playlist_t *playlist) {
    if (playlist) playlist->refs++;
}

static void playlist_release(hls_playlist_t *playlist) {
    if (!playlist || --playlist->refs > 0) return;
    free(playlist->text);
    free(playlist);
}

/* Whether the playlist carries `tag` at the start of any of its lines. */
static int playlist_has_tag(const hls_playlist_t *playlist, const char *tag) {
    size_t tag_len = strlen(tag);
    for (const char *p = playlist->text; p; p = next_line(p))
        if (strncmp(p, tag, tag_len) == 0) return 1;
    return 0;
}

/* The number in an "#EXT-X-SOMETHING:<n>" line, or 0 when there is none. */
static long playlist_tag_number(const hls_playlist_t *playlist,
                                const char *tag) {
    size_t tag_len = strlen(tag);
    for (const char *p = playlist->text; p; p = next_line(p))
        if (strncmp(p, tag, tag_len) == 0)
            return strtol(p + tag_len, NULL, 10);
    return 0;
}

/* Start of the index-th URI line (non-#, non-empty), or NULL. */
static const char *segment_line(const hls_playlist_t *playlist, int index) {
    if (!playlist || index < 0) return NULL;
    for (const char *p = playlist->text; p; p = next_line(p)) {
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (index-- == 0) return p;
    }
    return NULL;
}

static int segment_count(const hls_playlist_t *playlist) {
    int count = 0;
    for (const char *p = playlist->text; p; p = next_line(p))
        if (*p != '#' && *p != '\n' && *p != '\r' && *p) count++;
    return count;
}

/* The URI line following `url`, or NULL when there is none.
 *
 * *at_end (optional) tells the two ways that happens apart: `url` IS this
 * playlist's last segment, meaning the stream has not produced the next one
 * yet and a reload is what will name it — versus `url` not being in this
 * playlist at all, which is just a cursor working from a snapshot the window
 * has already moved past, and says nothing about the edge. */
static const char *segment_line_after(const hls_playlist_t *playlist,
                                      const char *url, int *at_end) {
    if (at_end) *at_end = 0;
    if (!playlist || !url) return NULL;
    size_t url_len = strlen(url);
    int seen = 0;
    for (const char *p = playlist->text; p; p = next_line(p)) {
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (seen) return p;
        if (strncmp(p, url, url_len) == 0 &&
            (p[url_len] == '\n' || p[url_len] == '\r' || !p[url_len]))
            seen = 1;
    }
    if (at_end) *at_end = seen;
    return NULL;
}

/* Copies the line at `p` up to its newline into a fresh allocation. Also used
 * for plain NUL-terminated strings, which are just lines without one. */
static char *line_dup(const char *line) {
    if (!line) return NULL;
    size_t len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r') len++;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, line, len);
    copy[len] = '\0';
    return copy;
}

/* Installs a freshly fetched playlist as the current one and re-reads the two
 * properties that decide how it is served. Takes ownership of `text`. Caller
 * holds worker_lock (except during init, before the worker exists). */
/* How much of a live stream the served playlist remembers. YouTube's window
 * tells the demuxer only what exists RIGHT NOW — as little as three segments
 * on some channels — while its segment URLs stay individually signed and
 * fetchable long after they scroll out (measured: half an hour). Serving the
 * raw window let a slow open land on a segment that expired mid-probe, and
 * libavformat answers that per cursor: one of the two skips ahead, the other
 * does not, and audio ends up a fixed two seconds in front of video for the
 * whole session — permanent stutter (console log 2026-09-01 21:08, twice,
 * only ever on 3-segment windows). So reloads APPEND to a local list instead
 * of replacing it: from the demuxer's side the playlist only grows, nothing
 * it is standing on ever expires, and the two cursors can never be split by
 * an expiry again.
 *
 * The cap is small on purpose, and not for memory: libavformat reparses the
 * WHOLE served playlist on every reload, on the demux thread — for audio,
 * that is the thread with a 23 ms deadline per output grain. An unbounded
 * list made that reparse grow with watch time, and the console showed it as
 * audio underruns whose rate and size crept up together (measured 2026-09-01
 * 22:45: au 30 in 5½ min, worst gap 32→53 ms, aj=0 — the stream itself was
 * clean). Thirty-two entries is minutes of no-expiry margin — playback
 * trails the edge by seconds — at a reparse cost that stays where it was in
 * the session's healthy first minute, forever. */
#ifndef HLS_ACC_MAX_SEGMENTS
#define HLS_ACC_MAX_SEGMENTS 32
#endif
#ifndef HLS_ACC_TRIM_BATCH
#define HLS_ACC_TRIM_BATCH 8
#endif

/* Start of the last URI line of `text`, or NULL. */
static const char *last_uri_line(const char *text) {
    const char *last = NULL;
    for (const char *p = text; p; p = next_line(p))
        if (*p && *p != '#' && *p != '\n' && *p != '\r') last = p;
    return last;
}

/* Start of the full line inside `text` whose content equals the line at
 * `line` (which may live in another buffer), or NULL. */
static const char *find_same_line(const char *text, const char *line) {
    size_t len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r') len++;
    if (!len) return NULL;
    for (const char *p = text; p; p = next_line(p))
        if (strncmp(p, line, len) == 0 &&
            (p[len] == '\n' || p[len] == '\r' || !p[len]))
            return p;
    return NULL;
}

/* The first segment-scoped line (the boundary between a playlist's header
 * and its media entries), or NULL for a playlist with no segments. */
static const char *first_entry_line(const char *text) {
    for (const char *p = text; p; p = next_line(p)) {
        if (strncmp(p, "#EXTINF:", 8) == 0 ||
            strncmp(p, "#EXT-X-DISCONTINUITY", 20) == 0 ||
            (*p && *p != '#' && *p != '\n' && *p != '\r'))
            return p;
    }
    return NULL;
}

/* Merges a freshly fetched live window (rewritten, NUL-terminated) into the
 * accumulated playlist. Returns a malloc'd merged text, or NULL when the old
 * text already covers the new window (caller keeps the old). */
static char *playlist_accumulate(const char *old_text, const char *new_text) {
    const char *new_last = last_uri_line(new_text);
    if (!new_last || find_same_line(old_text, new_last))
        return NULL; /* nothing newer in this window */
    const char *old_last = last_uri_line(old_text);
    const char *tail = NULL;
    const char *splice = "";
    if (old_last && (tail = find_same_line(new_text, old_last)) != NULL) {
        /* The common case: the window still overlaps what is held — append
         * exactly the entries that come after the overlap. */
        tail = next_line(tail);
    } else {
        /* The window slid past everything held (a stall longer than the
         * whole window). The missing stretch is a real hole in the stream
         * as served, so mark the seam; renumbering is not needed because
         * the demuxer addresses segments by URI, and the sequence numbers
         * only have to stay consistent within THIS text. */
        tail = first_entry_line(new_text);
        splice = "#EXT-X-DISCONTINUITY\n";
    }
    if (!tail) return NULL;
    size_t old_len = strlen(old_text);
    size_t add_len = strlen(splice) + strlen(tail);
    char *merged = malloc(old_len + add_len + 2);
    if (!merged) return NULL;
    memcpy(merged, old_text, old_len);
    char *w = merged + old_len;
    if (old_len && merged[old_len - 1] != '\n') *w++ = '\n';
    memcpy(w, splice, strlen(splice));
    w += strlen(splice);
    memcpy(w, tail, strlen(tail) + 1);
    return merged;
}

/* Bounds the accumulated list: drops HLS_ACC_TRIM_BATCH entries off the
 * front once the cap is passed, keeping MEDIA-SEQUENCE and
 * DISCONTINUITY-SEQUENCE consistent with what was dropped. Returns a
 * malloc'd trimmed text, or NULL when no trim is due. */
static char *playlist_trim(const char *text) {
    int uris = 0;
    for (const char *p = text; p; p = next_line(p))
        if (*p && *p != '#' && *p != '\n' && *p != '\r') uris++;
    if (uris <= HLS_ACC_MAX_SEGMENTS) return NULL;

    /* Walk to the end of the batch being dropped, counting what the header
     * counters must absorb. */
    const char *keep = first_entry_line(text);
    if (!keep) return NULL;
    int dropped = 0, dropped_disc = 0;
    const char *p = keep;
    while (p && dropped < HLS_ACC_TRIM_BATCH) {
        if (strncmp(p, "#EXT-X-DISCONTINUITY", 20) == 0 &&
            strncmp(p, "#EXT-X-DISCONTINUITY-SEQUENCE", 29) != 0)
            dropped_disc++;
        if (*p && *p != '#' && *p != '\n' && *p != '\r') dropped++;
        p = next_line(p);
    }
    if (!p) return NULL;
    keep = p;

    /* Rebuild the header with the two counters advanced. */
    size_t cap = strlen(text) + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t w = 0;
    for (p = text; p && p < keep && w < cap; p = next_line(p)) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (strncmp(p, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
            w += (size_t)snprintf(out + w, cap - w,
                                  "#EXT-X-MEDIA-SEQUENCE:%ld\n",
                                  strtol(p + 22, NULL, 10) + dropped);
        } else if (strncmp(p, "#EXT-X-DISCONTINUITY-SEQUENCE:", 30) == 0) {
            w += (size_t)snprintf(out + w, cap - w,
                                  "#EXT-X-DISCONTINUITY-SEQUENCE:%ld\n",
                                  strtol(p + 30, NULL, 10) + dropped_disc);
        } else if (strncmp(p, "#EXTINF:", 8) == 0 ||
                   (strncmp(p, "#EXT-X-DISCONTINUITY", 20) == 0 &&
                    strncmp(p, "#EXT-X-DISCONTINUITY-SEQUENCE", 29) != 0) ||
                   (*p && *p != '#' && *p != '\n' && *p != '\r')) {
            continue; /* a dropped entry's line */
        } else {
            memcpy(out + w, p, line_len);
            w += line_len;
        }
    }
    snprintf(out + w, cap - w, "%s", keep);
    return out;
}

static void playlist_install(VtHlsStreamFactory *factory, char *text) {
    rewrite_uris(text);
    /* The reload cadence answers "how often does the SERVER's window gain a
     * segment", so it is sized from the fetched window — before accumulation
     * grows the local copy far past it. */
    size_t fetched_len = strlen(text);

    /* A live source accumulates: see the block comment above
     * HLS_ACC_MAX_SEGMENTS. Everything else replaces. */
    if (factory->live && factory->playlist) {
        char *merged = playlist_accumulate(factory->playlist->text, text);
        if (merged) {
            free(text);
            text = merged;
        } else {
            /* This window brought no new segment. It may still carry the one
             * line that matters: a stream that ends publishes its last
             * segment in one refresh and #EXT-X-ENDLIST in the next, and that
             * second window is exactly what accumulate() calls "nothing new".
             * Without this the source stayed live forever — the demuxer never
             * saw the end, the player never reached EOF, and the last frame
             * froze with nothing but Circle to leave. Otherwise keep what is
             * held (a superset) and only refresh the clock. */
            const char *held = factory->playlist->text;
            int ended = strstr(text, "#EXT-X-ENDLIST") != NULL &&
                        strstr(held, "#EXT-X-ENDLIST") == NULL;
            free(text);
            text = NULL;
            if (ended) {
                static const char endlist[] = "#EXT-X-ENDLIST\n";
                size_t held_len = strlen(held);
                text = malloc(held_len + 1 + sizeof endlist);
                if (text) {
                    memcpy(text, held, held_len);
                    size_t at = held_len;
                    if (at && text[at - 1] != '\n') text[at++] = '\n';
                    memcpy(text + at, endlist, sizeof endlist);
                }
            }
            if (!text) {
                factory->playlist_time_us = sceKernelGetProcessTimeWide();
                return;
            }
        }
        char *trimmed = playlist_trim(text);
        if (trimmed) {
            free(text);
            text = trimmed;
        }
    }

    hls_playlist_t *playlist = playlist_new(text);
    if (!playlist) return; /* keep the old one: stale beats none */
    playlist_release(factory->playlist);
    factory->playlist = playlist;
    factory->playlist_time_us = sceKernelGetProcessTimeWide();
    /* A playlist that has ended is complete and never has to be looked at
     * again; one that has not is a window that moves, and every reload of it
     * is another fetch. This is the single thing that tells live from on
     * demand here — the manifest URL, the codecs and the segments are the
     * same shape either way. */
    factory->live = !playlist_has_tag(playlist, "#EXT-X-ENDLIST");
    long target_s = playlist_tag_number(playlist, "#EXT-X-TARGETDURATION:");
    /* One reload per segment duration — the rate at which the window gains
     * anything new — with a two-second floor. (Halving the target duration,
     * an earlier draft, meant a round trip every second to learn nothing.) */
    uint64_t refresh_us = target_s > 0 ? (uint64_t)target_s * 1000000ULL
                                       : 3000000ULL;
    if (refresh_us < 2000000ULL) refresh_us = 2000000ULL;
    /* ...but never so often that the reload becomes the load: hold the
     * reload traffic to roughly 20 KB/s if a server ever hands out a huge
     * window. Sized from the fetch, not from the accumulated copy. */
    uint64_t by_size_us = (uint64_t)fetched_len * 50ULL;
    if (by_size_us > refresh_us) refresh_us = by_size_us;
    if (refresh_us > 60000000ULL) refresh_us = 60000000ULL;
    factory->refresh_after_us = (unsigned)refresh_us;
}

/* ----------------------------------------------------------------- cursors */

typedef struct {
    VtHlsStreamFactory *owner;
    hls_playlist_t *playlist; /* the snapshot this cursor was opened on */
    size_t pos;
} hls_cursor_t;

static int cursor_read(void *opaque, void *buffer, size_t size) {
    hls_cursor_t *cursor = opaque;
    if (!cursor || !cursor->playlist) return -1;
    const hls_playlist_t *playlist = cursor->playlist;
    if (cursor->pos >= playlist->len) return 0; /* EOF */
    size_t left = playlist->len - cursor->pos;
    size_t take = size < left ? size : left;
    memcpy(buffer, playlist->text + cursor->pos, take);
    cursor->pos += take;
    return (int)take;
}

static int64_t cursor_seek(void *opaque, int64_t offset, int whence) {
    hls_cursor_t *cursor = opaque;
    if (!cursor || !cursor->playlist) return -1;
    int64_t size = (int64_t)cursor->playlist->len;
    int64_t target = offset;
    if (whence == SEEK_CUR) target = (int64_t)cursor->pos + offset;
    else if (whence == SEEK_END) target = size + offset;
    if (target < 0 || target > size) return -1;
    cursor->pos = (size_t)target;
    return target;
}

/* Gives `cursor` the window this decoder session reads. Caller holds
 * worker_lock (or the worker does not exist yet).
 *
 * Every cursor of one session must read the SAME bytes. libavformat works out
 * where to join a live stream from the playlist in front of it — three
 * segments back from its edge — and the decoder opens its two cursors one
 * after the other, with a segment download and a probe in between. Hand each
 * of them whatever was current at its own open and the second one joins a
 * segment later than the first: audio then runs ~5 s ahead of video for the
 * rest of the session, and because the two are reading different segments
 * they stop sharing downloads as well, which doubles the bandwidth of the one
 * design that exists to halve it. Pinning at the first cursor of a session
 * costs nothing and makes both of those impossible. */
static void factory_reset_speculation(VtHlsStreamFactory *factory);

static void cursor_attach(VtHlsStreamFactory *factory, hls_cursor_t *cursor) {
    if (factory->open_cursors == 0) {
        playlist_release(factory->session_playlist);
        factory->session_playlist = factory->playlist;
        playlist_retain(factory->session_playlist);
        /* A new session (seek, recovery) starts somewhere else: what the
         * previous one had read ahead and never consumed would otherwise sit
         * in the ring as "pending read-ahead" forever — nothing evicts an
         * unread slot — and hold the chain at depth 1 for the rest of
         * playback. Drop the stale chain and the unread slots; the very
         * first session keeps its init-time seed. */
        if (factory->sessions > 0) factory_reset_speculation(factory);
        factory->sessions++;
    }
    factory->open_cursors++;
    cursor->playlist = factory->session_playlist;
    playlist_retain(cursor->playlist);
}

static void cursor_detach(VtHlsStreamFactory *factory, hls_cursor_t *cursor) {
    if (factory->open_cursors > 0) factory->open_cursors--;
    playlist_release(cursor->playlist);
}

static void cursor_close(void *opaque) {
    hls_cursor_t *cursor = opaque;
    if (!cursor) return;
    VtHlsStreamFactory *factory = cursor->owner;
    if (factory && factory->worker_started) {
        pthread_mutex_lock(&factory->worker_lock);
        cursor_detach(factory, cursor);
        pthread_mutex_unlock(&factory->worker_lock);
    } else if (factory) {
        cursor_detach(factory, cursor);
    } else {
        playlist_release(cursor->playlist);
    }
    free(cursor);
}

/* --------------------------------------------------- segment worker ------
 *
 * Fetching a segment synchronously from cursor_fetch stalled the demux
 * thread — and with it decode and, on the audio cursor, the playback clock —
 * for the full HTTPS round trip at every ~5 s segment boundary. On real
 * hardware that was a visible CPU spike plus a micro-stutter every few
 * seconds (measured 2026-08-29: media/wall ~0.94x, the presentation clock
 * losing ~300 ms per segment, ready frames dipping to 0 at each boundary).
 *
 * Each factory therefore runs one worker thread that owns factory->client
 * exclusively after init (vita_https clients stay single-owner). The worker
 * serves demanded segments and, once a demand is satisfied, fetches the
 * FOLLOWING segment on its own — by the time the demuxer crosses the
 * boundary, the bytes are already in memory and the handoff is a copy out of
 * the ring. It also owns the playlist reloads a live source needs, for the
 * same reason: a cursor thread must never sit on the network.
 *
 * Segments are addressed by URL, not by position: a live playlist renumbers
 * itself every time it slides, and the two cursors of a muxed source can be
 * looking at snapshots taken seconds apart, while a segment's URL is the same
 * string in both. The demux threads poll (2 ms, same pattern as the decoder's
 * reserve_slot) instead of waiting on the condvar so a cancel can always get
 * through. */

typedef enum {
    HLS_CACHE_EMPTY = 0,
    HLS_CACHE_LOADING,
    HLS_CACHE_READY,
    HLS_CACHE_FAILED
} hls_cache_state_t;

#define HLS_WORKER_STACK (256 * 1024) /* curl/TLS headroom, like elementary_audio */
#define HLS_WORKER_POLL_US 2000
/* Longer than the 30 s per-request cap, so a fetch that is merely slow still
 * gets to finish; this only catches a worker that will never answer. */
#define HLS_SERVE_TIMEOUT_US (40 * 1000000)
/* Consecutive failed live reloads before serve_playlist reports the outage
 * (one reload per target duration, so ~15-25 s at YouTube's 2-5 s segments). */
#define HLS_RELOAD_FAILURE_LIMIT 5
/* How long a cursor's "gave up on this segment" verdict binds the other one. */
#define HLS_ABANDON_TTL_US (60ULL * 1000000ULL)
/* How often an otherwise idle worker looks for work on a live source. Short
 * on purpose, and not a condvar: a worker that just finished a fetch re-picks
 * the freshly extended chain immediately, so its idle sibling only ever gets
 * a segment by noticing next_url within one fetch's duration — that overlap
 * IS the second connection's contribution. 10 ms against fetches that take
 * hundreds guarantees it; the idle cost is a hundred brief lock-takes a
 * second. (The age check for the reload rides on the same poll.) */
#define HLS_IDLE_POLL_US 10000

/* What the ring may hold in total. Bounding it by bytes rather than by slot
 * count is what makes one ring fit both shapes: a live segment is a few
 * hundred KB and all four fit, while an on-demand one can be 3 MB and is read
 * by a single cursor that never looks back — four of those would be megabytes
 * of cache nobody asks for again. */
/* Live segments run 1-2 MB apiece on a 5-second window (measured 22 MB for
 * 19 of them, 2026-09-01), and this budget does double duty: slots_trim
 * evicts past it, and the read-ahead stops extending at half of it. At 8 MB
 * that half was ~3 such segments — the intended depth of 6 was never
 * reached, and the ring then had nothing to spare when the trailing cursor
 * came asking. The console has a 128 MB heap; 16 in flight of video data is
 * a bargain against a one-second stall every minute. */
#ifndef HLS_CACHE_BUDGET
#define HLS_CACHE_BUDGET (16 * 1024 * 1024)
#endif

/* How many fetched-but-unserved segments the read-ahead tries to keep. One
 * was the old depth, which is two seconds of lead on a live window — nothing
 * to absorb a slow fetch with, and it left the link idle between demands
 * instead of turning that idle time into buffer. */
#define HLS_READAHEAD_SEGMENTS 6

/* All of these take worker_lock held. */
static void slot_clear(VtHlsSegmentSlot *slot) {
    free(slot->url);
    slot->url = NULL;
    free(slot->data);
    slot->data = NULL;
    slot->size = 0;
    slot->state = HLS_CACHE_EMPTY;
}

/* A new decoder session on this factory (see cursor_attach): drop the
 * speculative chain and every read-ahead slot no cursor has consumed. */
static void factory_reset_speculation(VtHlsStreamFactory *factory) {
    free(factory->next_url);
    factory->next_url = NULL;
    free(factory->last_url);
    factory->last_url = NULL;
    for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++)
        if (factory->slots[i].state == HLS_CACHE_READY &&
            factory->slots[i].serves == 0)
            slot_clear(&factory->slots[i]);
}

static int slot_find(VtHlsStreamFactory *factory, const char *url) {
    for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++)
        if (factory->slots[i].url && strcmp(factory->slots[i].url, url) == 0)
            return i;
    return -1;
}

/* Claims the least recently used slot for `url` and marks it loading. Never
 * takes one that is mid-fetch. Returns its index, or -1. */
static int slot_reserve(VtHlsStreamFactory *factory, const char *url) {
    int best = -1;
    for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++) {
        VtHlsSegmentSlot *slot = &factory->slots[i];
        if (slot->state == HLS_CACHE_LOADING) continue;
        if (!slot->url) { best = i; break; }
        if (best < 0 || slot->stamp < factory->slots[best].stamp) best = i;
    }
    if (best < 0) return -1;
    slot_clear(&factory->slots[best]);
    factory->slots[best].url = line_dup(url);
    if (!factory->slots[best].url) return -1;
    factory->slots[best].state = HLS_CACHE_LOADING;
    factory->slots[best].stamp = ++factory->stamp_counter;
    return best;
}

/* How much actual read-ahead exists: segments in flight, plus fetched ones no
 * cursor has consumed yet. Consumed segments stay READY in the ring — that is
 * how a muxed source's second cursor gets its copy — but counting them here
 * froze the read-ahead solid: after the first handful of serves the ring
 * always held enough leftovers to satisfy the depth check, the chain never
 * extended again, and every following segment became an on-demand fetch with
 * the demuxer standing still through its full first-byte latency. That is the
 * live-edge starvation the console logs showed (2026-09-01, evening), and it
 * is also why the one session that opened SLOWLY played perfectly: it started
 * a whole window behind the edge, where a stalled read-ahead has a backlog to
 * hide in. bytes_out still reports every READY slot's bytes — leftovers do
 * occupy the budget. In-flight counts: with two workers, a depth check that
 * ignored LOADING slots would let both keep starting new fetches past the
 * intended depth. Caller holds worker_lock. */
static int fresh_pending(const VtHlsStreamFactory *factory, size_t *bytes_out) {
    int n = 0;
    size_t bytes = 0;
    for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++) {
        if (factory->slots[i].state == HLS_CACHE_LOADING) {
            n++;
        } else if (factory->slots[i].state == HLS_CACHE_READY) {
            if (factory->slots[i].serves == 0) n++;
            bytes += factory->slots[i].size;
        }
    }
    if (bytes_out) *bytes_out = bytes;
    return n;
}

/* Drops segments until the ring is back inside its budget. Only touches
 * finished ones: a slot mid-fetch has no size yet, and a slot being copied
 * out is protected by the lock this is called under.
 *
 * Eviction order is by USEFULNESS, then age. A segment every open cursor has
 * consumed is dead weight and goes first. Plain LRU here was a real stutter
 * on the console (2026-09-01 21:56, ~1 s freeze with a FULL frame queue):
 * the segment the trailing cursor was about to ask for had the oldest stamp
 * — served once, long ago by ring standards — so LRU evicted exactly it,
 * and the trailer paid a full network round trip for bytes the ring had
 * already held. */
static void slots_trim(VtHlsStreamFactory *factory) {
    for (;;) {
        size_t total = 0;
        int victim = -1, victim_rank = 0, ready = 0;
        for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++) {
            VtHlsSegmentSlot *slot = &factory->slots[i];
            if (slot->state != HLS_CACHE_READY) continue;
            ready++;
            total += slot->size;
            /* rank 2: fully consumed; 1: partly; 0: still unread. */
            int rank = slot->serves >= factory->open_cursors ? 2
                     : slot->serves > 0 ? 1 : 0;
            if (victim < 0 || rank > victim_rank ||
                (rank == victim_rank &&
                 slot->stamp < factory->slots[victim].stamp)) {
                victim = i;
                victim_rank = rank;
            }
        }
        /* Never evict the last one standing: it is what the cursor that
         * triggered this fetch is about to collect. */
        if (victim < 0 || ready <= 1 || total <= HLS_CACHE_BUDGET) return;
        if (victim_rank < 2)
            /* Forced to drop something a cursor still needs — that is a
             * refetch and a stall in the making, worth seeing in the log. */
            vt_log("hls: budget evicts a segment consumed %d/%d times",
                   factory->slots[victim].serves, factory->open_cursors);
        slot_clear(&factory->slots[victim]);
    }
}

/* Fetches an absolute playlist entry with the factory's client. The URL is
 * stored with the file: scheme rewrite_uris() applied and has to go back to
 * https: to be fetched. Worker thread only. */
static int fetch_segment(VtHlsStreamFactory *factory,
                         struct VitaHttpsClient *client, const char *url,
                         fetch_buf_t *out) {
    /* "https://" is one byte longer than the stored "file://", so the rebuilt
     * URL needs that byte of extra headroom over the stored one. */
    char real[4104];
    if (strncmp(url, "file://", 7) == 0)
        snprintf(real, sizeof real, "https://%s", url + 7);
    else
        snprintf(real, sizeof real, "%s", url);
    /* Known quirk (device logs 2026-09-05): googlevideo answers the very
     * first request for segment 0 of a freshly generated playlist with 404
     * now and then; the demand a moment later succeeds. serve_segment's
     * retry covers it, so a failed read-ahead here is not worth a log. */
    return fetch_into(client, real, factory->cancel_flag, out);
}

/* Whether a live window has aged past the point where FFmpeg's next reload
 * would learn nothing new from it. Caller holds worker_lock. */
static int playlist_stale(const VtHlsStreamFactory *factory) {
    return factory->live &&
           sceKernelGetProcessTimeWide() - factory->playlist_time_us >=
               factory->refresh_after_us;
}

/* Re-fetches the media playlist and installs it. Worker thread only; called
 * with worker_lock NOT held. */
/* Returns 0 when a playlist was installed, -1 when the reload failed. */
static int playlist_refetch(VtHlsStreamFactory *factory,
                            struct VitaHttpsClient *client) {
    fetch_buf_t buf = {0};
    int ret = fetch_into(client, factory->url, factory->cancel_flag, &buf);
    pthread_mutex_lock(&factory->worker_lock);
    factory->reloads++;
    unsigned failures = 0;
    if (ret == 0 && buf.len) {
        playlist_install(factory, buf.data);
        factory->reload_failures = 0;
    } else {
        free(buf.data);
        failures = ++factory->reload_failures;
        /* Do not retry instantly: the age check would still say stale and the
         * worker would spin on a failing fetch. Pretend it just arrived; the
         * next interval tries again. */
        factory->playlist_time_us = sceKernelGetProcessTimeWide();
    }
    pthread_mutex_unlock(&factory->worker_lock);
    /* Outside the lock (card I/O), and not once per interval for the whole
     * outage: the first few and then every twentieth. */
    if (failures && (failures <= 3 || failures % 20 == 0))
        vt_log("hls: playlist reload failed (ret=%d, %u in a row)", ret,
               failures);
    return failures ? -1 : 0;
}

/* Fetches `target` (adopted) into a ring slot. Called with worker_lock held;
 * drops it for the transfer and takes it again. */
static void worker_fetch(VtHlsStreamFactory *factory,
                         struct VitaHttpsClient *client, char *target) {
    if (slot_find(factory, target) >= 0) {
        /* Already cached — but never let the CHAIN die on that: a chain
         * re-seeded from a just-served segment points at cached ones by
         * construction, and dropping it here left the read-ahead dead until
         * the next serve re-seeded it onto another cached segment, forever.
         * Walk forward instead; the loop re-picks next_url immediately and
         * the walk ends at the first segment actually worth fetching. */
        if (!factory->next_url) {
            const char *next =
                segment_line_after(factory->playlist, target, NULL);
            if (next) factory->next_url = line_dup(next);
            /* At the window's end the chain PARKS — it must never request a
             * reload. It did once, and the result was a reload spin: reload,
             * re-seed, walk back to the unchanged edge, request again — the
             * workers spent their entire time on playlist round trips while
             * segment fetches queued behind them, which on the console reads
             * as a live stream starving at the very edge. The cadence reload
             * revives a parked chain via last_url. */
        }
        free(target);
        return;
    }
    int slot = slot_reserve(factory, target);
    if (slot < 0) {
        free(target);
        return;
    }
    factory->slots[slot].serves = 0;
    pthread_mutex_unlock(&factory->worker_lock);

    fetch_buf_t buf = {0};
    int ret = fetch_segment(factory, client, target, &buf);

    pthread_mutex_lock(&factory->worker_lock);
    if (factory->slots[slot].state == HLS_CACHE_LOADING &&
        factory->slots[slot].url &&
        strcmp(factory->slots[slot].url, target) == 0) {
        if (ret == 0 && buf.len) {
            factory->slots[slot].data = buf.data;
            factory->slots[slot].size = buf.len;
            factory->slots[slot].state = HLS_CACHE_READY;
            factory->downloads++;
            factory->bytes += buf.len;
            slots_trim(factory);
            /* Keep going while the ring has room: the spare time between one
             * demand and the next is exactly what should become buffer. */
            size_t held = 0;
            /* Stop well short of the byte budget: past it slots_trim starts
             * evicting, and the oldest READY slot is exactly the one a cursor
             * that has fallen behind is about to ask for. Reading ahead into
             * that is worse than not reading ahead at all. */
            if (!factory->next_url &&
                fresh_pending(factory, &held) < HLS_READAHEAD_SEGMENTS &&
                held * 2 < HLS_CACHE_BUDGET) {
                /* Parks at the window's end, same as the other two sites. */
                const char *next =
                    segment_line_after(factory->playlist, target, NULL);
                if (next) factory->next_url = line_dup(next);
            }
        } else {
            free(buf.data);
            factory->slots[slot].state = HLS_CACHE_FAILED;
        }
    } else {
        free(buf.data); /* the slot was recycled under us: result is stale */
    }
    free(target);
}

static void *hls_worker(void *opaque) {
    struct VtHlsWorkerSlot *self = opaque;
    VtHlsStreamFactory *factory = self->factory;
    /* Below the main thread (160): segment TLS decrypt must never preempt
     * UI or audio delivery — the whole point is moving that work off the
     * playback-critical path. */
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 168);
    pthread_mutex_lock(&factory->worker_lock);
    while (!factory->worker_stop) {
        /* A cursor blocked on a segment comes first: a demux thread is
         * standing still until it arrives. */
        if (factory->want_url) {
            char *target = factory->want_url;
            factory->want_url = NULL;
            worker_fetch(factory, self->client, target);
            continue;
        }
        /* Then the window itself — and, for a live source, whether or not
         * anyone has asked. Reloading only on demand meant reloading with a
         * demux thread stopped in front of it, behind whatever segment
         * download happened to be in flight: seconds of stall every few
         * seconds. Doing it here, while nothing is waiting, costs 8 KB and
         * no stall at all. One worker at a time: the other would only fetch
         * the same bytes again. */
        if ((factory->want_playlist || playlist_stale(factory)) &&
            !factory->reload_in_flight) {
            factory->reload_in_flight = 1;
            pthread_mutex_unlock(&factory->worker_lock);
            playlist_refetch(factory, self->client);
            pthread_mutex_lock(&factory->worker_lock);
            factory->reload_in_flight = 0;
            /* Cleared after the fetch, not before: a cursor asking for a
             * reload while this one was in flight is asking for the same
             * reload — clearing early would queue a second fetch of a
             * playlist that just arrived. */
            factory->want_playlist = 0;
            /* The window may finally name the segment after the last one
             * served — that is exactly what an edge looks like once it has
             * moved on. */
            if (!factory->next_url && factory->last_url) {
                const char *next = segment_line_after(factory->playlist,
                                                      factory->last_url, NULL);
                if (next) factory->next_url = line_dup(next);
            }
            continue;
        }
        /* Then read-ahead, so upcoming segments are in memory before the
         * demuxer crosses each boundary. */
        if (factory->next_url) {
            char *target = factory->next_url;
            factory->next_url = NULL;
            /* Extend the chain BEFORE fetching, not after: the whole point
             * of the second worker is that it can pick the following segment
             * up while this one is still on the wire. The target being taken
             * is not LOADING yet, hence the +1. A stale chain that points at
             * an already-cached segment just walks forward one dedupe at a
             * time — worker_fetch drops duplicates via slot_find. */
            size_t held = 0;
            if (fresh_pending(factory, &held) + 1 < HLS_READAHEAD_SEGMENTS &&
                held * 2 < HLS_CACHE_BUDGET) {
                /* Parks at the window's end — never a reload request from
                 * the chain; see the dedupe path in worker_fetch. */
                const char *next =
                    segment_line_after(factory->playlist, target, NULL);
                if (next) factory->next_url = line_dup(next);
            }
            worker_fetch(factory, self->client, target);
            continue;
        }
        if (factory->live) {
            /* Come back and check the window's age. A condvar wait would
             * only ever be woken by a demand, and the reload above has to
             * happen before there is one. */
            pthread_mutex_unlock(&factory->worker_lock);
            sceKernelDelayThread(HLS_IDLE_POLL_US);
            pthread_mutex_lock(&factory->worker_lock);
        } else {
            /* Nothing to do, and a complete playlist never changes: every
             * demand is served and waiting for a cursor to collect it. */
            pthread_cond_wait(&factory->worker_wake, &factory->worker_lock);
        }
    }
    pthread_mutex_unlock(&factory->worker_lock);
    return NULL;
}

/* Hands the demuxer the current playlist. For a live source that means the
 * freshest one the worker can get: FFmpeg only ever reloads a playlist to
 * learn about segments that did not exist when it read the last one, so
 * answering with a stale copy sends it round another wait for nothing. */
static int serve_playlist(VtHlsStreamFactory *factory, unsigned char **data,
                          size_t *size) {
    pthread_mutex_lock(&factory->worker_lock);
    /* Never waits. The worker keeps a live window fresh on its own, so what
     * is here is what a fetch would have returned anyway — and if it has
     * aged after all, asking for a refresh and serving the copy in hand
     * still beats standing still: FFmpeg answers a window with nothing new
     * by sleeping 100 ms and asking again, while a fetch queued behind a
     * segment download holds the demux thread for seconds. */
    if (playlist_stale(factory)) {
        factory->want_playlist = 1;
        pthread_cond_signal(&factory->worker_wake);
    }
    if (!factory->playlist) {
        pthread_mutex_unlock(&factory->worker_lock);
        return -1;
    }
    /* A live window that could not be refreshed for a while is not "what a
     * fetch would have returned": serving it kept a dead network invisible
     * for as long as the demuxer's reload budget lasts (tens of minutes of
     * frozen picture, then a silent "video finished"). Refusing it makes
     * the outage an error, and the player's live recovery takes over. */
    if (factory->live &&
        factory->reload_failures >= HLS_RELOAD_FAILURE_LIMIT) {
        unsigned failures = factory->reload_failures;
        pthread_mutex_unlock(&factory->worker_lock);
        vt_log("hls: playlist unavailable after %u failed reloads, "
               "reporting the outage", failures);
        return -1;
    }
    unsigned char *copy = malloc(factory->playlist->len + 1);
    if (!copy) {
        pthread_mutex_unlock(&factory->worker_lock);
        return -1;
    }
    memcpy(copy, factory->playlist->text, factory->playlist->len + 1);
    *data = copy;
    *size = factory->playlist->len;
    pthread_mutex_unlock(&factory->worker_lock);
    return 0;
}

static int serve_segment(VtHlsStreamFactory *factory, const char *url,
                         unsigned char **data, size_t *size) {
    int attempts = 0;
    int waited_us = 0;
    pthread_mutex_lock(&factory->worker_lock);
    /* The other cursor gave this segment up moments ago: the demuxer skips a
     * segment whose open fails, and the two cursors must skip the SAME one
     * or audio and video part ways for good. */
    if (factory->abandoned_url && strcmp(factory->abandoned_url, url) == 0 &&
        sceKernelGetProcessTimeWide() - factory->abandoned_at_us <
            HLS_ABANDON_TTL_US) {
        pthread_mutex_unlock(&factory->worker_lock);
        vt_log("hls: segment skipped with the other cursor: %.60s", url);
        return -1;
    }
    for (;;) {
        if (factory->cancel_flag && *factory->cancel_flag) {
            pthread_mutex_unlock(&factory->worker_lock);
            return -1;
        }
        int slot = slot_find(factory, url);
        if (slot >= 0 && factory->slots[slot].state == HLS_CACHE_READY) {
            /* Copied out rather than handed over: a muxed playlist is read by
             * BOTH cursors, so the same segment is asked for twice, a beat
             * apart — and the demuxer frees what it is given, so it can never
             * be the ring's own buffer. */
            unsigned char *copy = malloc(factory->slots[slot].size);
            if (!copy) {
                pthread_mutex_unlock(&factory->worker_lock);
                return -1;
            }
            memcpy(copy, factory->slots[slot].data, factory->slots[slot].size);
            *data = copy;
            *size = factory->slots[slot].size;
            factory->slots[slot].stamp = ++factory->stamp_counter;
            /* Consumed (once more): it stops counting as read-ahead, and
             * once every open cursor has taken it, eviction may have it. */
            factory->slots[slot].serves++;
            factory->served++;
            factory->consecutive_failures = 0;
            break;
        }
        if (slot >= 0 && factory->slots[slot].state == HLS_CACHE_FAILED) {
            /* A failure here may just be the PREVIOUS session's abort (a seek
             * cancels in-flight fetches and then reopens the same factory),
             * so the first hit retries once before it becomes the demuxer's
             * problem. */
            slot_clear(&factory->slots[slot]);
            if (++attempts > 1) {
                free(factory->abandoned_url);
                factory->abandoned_url = line_dup(url);
                factory->abandoned_at_us = sceKernelGetProcessTimeWide();
                unsigned failures = ++factory->consecutive_failures;
                pthread_mutex_unlock(&factory->worker_lock);
                if (failures <= 3)
                    vt_log("hls: segment failed twice (%u in a row): %.60s",
                           failures, url);
                return -1;
            }
            slot = -1;
        }
        /* Only claim the demand slot when it is free: the other cursor may
         * have just put its own segment there, and overwriting that would
         * leave it polling for something nobody is fetching. */
        if (slot < 0 && !factory->want_url) {
            factory->want_url = line_dup(url);
            pthread_cond_signal(&factory->worker_wake);
        }
        pthread_mutex_unlock(&factory->worker_lock);
        sceKernelDelayThread(HLS_WORKER_POLL_US);
        pthread_mutex_lock(&factory->worker_lock);
        waited_us += HLS_WORKER_POLL_US;
        if (waited_us >= HLS_SERVE_TIMEOUT_US) {
            free(factory->abandoned_url);
            factory->abandoned_url = line_dup(url);
            factory->abandoned_at_us = sceKernelGetProcessTimeWide();
            factory->consecutive_failures++;
            pthread_mutex_unlock(&factory->worker_lock);
            vt_log("hls: segment timed out: %.60s", url);
            return -1;
        }
    }
    /* Line up the next segment before the demuxer wants it — or, when this
     * was the last one the window names, the reload that will name it. */
    free(factory->last_url);
    factory->last_url = line_dup(url);
    int at_end = 0;
    const char *next = segment_line_after(factory->playlist, url, &at_end);
    if (next) {
        free(factory->next_url);
        factory->next_url = line_dup(next);
    } else if (factory->live && at_end) {
        factory->want_playlist = 1;
    }
    pthread_cond_signal(&factory->worker_wake);
    /* The one number that says whether the shared ring is doing its job: a
     * muxed source is read by two cursors, so "served" should come out at
     * twice "downloaded"; anything near 1:1 means the stream is being pulled
     * twice over a link that has no room for that. Snapshot under the lock,
     * write AFTER it: this runs on the audio master-clock thread with a 23 ms
     * deadline, and a synchronous card write while holding worker_lock
     * stalled both cursors and both workers — the device log paired 17 of 21
     * of these lines with an audio underrun. */
    int log_now = factory->served / 20 > factory->last_logged_serves / 20;
    unsigned served = factory->served, downloads = factory->downloads;
    unsigned reloads = factory->reloads;
    unsigned kb = (unsigned)(factory->bytes / 1024);
    if (log_now) factory->last_logged_serves = factory->served;
    pthread_mutex_unlock(&factory->worker_lock);
    if (log_now)
        vt_log("hls: %u served / %u downloaded / %u reloads / %u KB", served,
               downloads, reloads, kb);
    return 0;
}

/* Serves everything the demuxer opens through the playlist: its segments, and
 * — for a live source — the playlist itself, every time FFmpeg reloads it to
 * see what has appeared since. Both arrive with the file: scheme
 * rewrite_uris() applied, and because every URI in the playlist is absolute,
 * a segment URL is byte-identical to its playlist line. */
static int cursor_fetch(void *opaque, const char *url, unsigned char **data,
                        size_t *size) {
    hls_cursor_t *cursor = opaque;
    if (!cursor || !url || !data || !size) return -1;
    VtHlsStreamFactory *factory = cursor->owner;
    *data = NULL;
    *size = 0;
    if (!factory->worker_started) {
        vt_log("hls: fetch before the worker started");
        return -1;
    }
    if (strcmp(url, factory->decoder_url) == 0)
        return serve_playlist(factory, data, size);
    return serve_segment(factory, url, data, size);
}

static int hls_open(void *opaque, VtDecoderStreamHandle *out) {
    VtHlsStreamFactory *factory = opaque;
    if (!factory || !out) return -1;
    hls_cursor_t *cursor = calloc(1, sizeof *cursor);
    if (!cursor) return -1;
    cursor->owner = factory;
    /* Everything read here can be replaced by a worker's reload mid-call, so
     * even the "is there a playlist at all" look happens under the lock. */
    if (factory->worker_started) {
        pthread_mutex_lock(&factory->worker_lock);
        if (!factory->playlist) {
            pthread_mutex_unlock(&factory->worker_lock);
            free(cursor);
            return -1;
        }
        cursor_attach(factory, cursor);
        pthread_mutex_unlock(&factory->worker_lock);
    } else {
        if (!factory->playlist) {
            free(cursor);
            return -1;
        }
        cursor_attach(factory, cursor);
    }
    memset(out, 0, sizeof *out);   /* see remote_open */
    out->opaque = cursor;
    out->read = cursor_read;
    out->seek = cursor_seek;
    out->close = cursor_close;
    out->size = (int64_t)cursor->playlist->len;
    out->fetch = cursor_fetch;
    out->url = factory->decoder_url;
    return 0;
}

int vt_hls_stream_factory_init(VtHlsStreamFactory *factory, const char *url,
                               volatile int *cancel_flag) {
    memset(factory, 0, sizeof *factory);
    snprintf(factory->url, sizeof factory->url, "%s", url);
    /* The demuxer needs the playlist's own URL in the same scheme as the
     * rewritten URIs inside it, so that resolving an entry against it stays
     * within file: and lands in the fetch callback — and so that a live
     * reload, which asks for that very URL, is recognisable there. */
    if (strncmp(url, "https://", 8) == 0)
        snprintf(factory->decoder_url, sizeof factory->decoder_url,
                 "file://%s", url + 8);
    else
        snprintf(factory->decoder_url, sizeof factory->decoder_url, "%s", url);
    factory->cancel_flag = cancel_flag;

    VitaHttpsClientConfig config = {
        .user_agent = HLS_USER_AGENT,
        .connect_timeout_ms = 10000,
        .request_timeout_ms = 30000, /* caps one segment fetch */
        /* NOT persistent, deliberately. Reusing one handle per worker would
         * save DNS + TCP + two TLS round trips per segment, but on the
         * console (2026-09-05, two device logs) the first segment fetched
         * after the playlist on a reused handle answered HTTP 404 from
         * googlevideo — and once, with no recovery, every segment after it
         * did, for as long as the playlist was. Fresh handles never showed a
         * 404 in any device log. Until that is understood, correctness wins
         * over the handshake. */
        .persistent_connection = 0,
    };
    factory->client = vita_https_client_create(&config);
    if (!factory->client) return -1;

    fetch_buf_t buf = {0};
    int ret = fetch_into(factory->client, factory->url, factory->cancel_flag,
                         &buf);
    if (ret < 0 || !buf.len) {
        free(buf.data);
        vt_hls_stream_factory_term(factory);
        return ret < 0 ? ret : -1;
    }
    playlist_install(factory, buf.data);
    if (!factory->playlist) {
        vt_hls_stream_factory_term(factory);
        return -1;
    }
    factory->factory.opaque = factory;
    factory->factory.open = hls_open;

    /* Aim the read-ahead at the segment the demuxer will actually open first.
     * Seeded BEFORE the workers exist, on purpose: this touches
     * factory->playlist without the lock, and the moment a worker runs it may
     * reload and REPLACE that playlist — the chain reaching the live edge
     * queues exactly such a reload. Seeding afterwards was a use-after-free
     * with a two-worker head start (caught by TSan, and almost certainly the
     * host test's crash). The workers pick the seed up on their first loop
     * iteration, so nothing needs signaling either.
     *
     * The seed itself: this init runs on the prepare worker while the OLD
     * source is still playing, so the segment downloads in the background
     * during stage 2 and the exclusive commit step later finds it already in
     * memory. For a finished playlist that first segment is the top of the
     * list; a live one is joined near the END of its window instead (see
     * live_start_index in vita_hw_decoder.c), so reading ahead from the top
     * would fetch something nobody asks for. */
    int first = 0;
    if (factory->live) {
        /* Matches live_start_index -5 (vita_hw_decoder.c): the seed must be
         * the segment playback will actually demand first. */
        int count = segment_count(factory->playlist);
        first = count > 5 ? count - 5 : 0;
    }
    factory->next_url = line_dup(segment_line(factory->playlist, first));

    vt_log("hls: playlist ready, %u bytes, %d segments, %s",
           (unsigned)factory->playlist->len, segment_count(factory->playlist),
           factory->live ? "live" : "complete");

    /* From here on each worker is its own client's only user (hls_worker).
     * Live gets a second connection; see the workers comment in hls.h. */
    factory->workers[0].factory = factory;
    factory->workers[0].client = factory->client;
    int workers_wanted = 1;
    if (factory->live) {
        factory->client2 = vita_https_client_create(&config);
        if (factory->client2) {
            factory->workers[1].factory = factory;
            factory->workers[1].client = factory->client2;
            workers_wanted = 2;
        } else {
            vt_log("hls: no second client, live rides one connection");
        }
    }
    int ret2 = pthread_mutex_init(&factory->worker_lock, NULL);
    if (ret2 == 0) {
        ret2 = pthread_cond_init(&factory->worker_wake, NULL);
        if (ret2 != 0) pthread_mutex_destroy(&factory->worker_lock);
    }
    if (ret2 == 0) {
        pthread_attr_t attr;
        int attr_ready = pthread_attr_init(&attr) == 0;
        if (attr_ready) pthread_attr_setstacksize(&attr, HLS_WORKER_STACK);
        for (int i = 0; i < workers_wanted; i++) {
            int r = pthread_create(&factory->workers[i].thread,
                                   attr_ready ? &attr : NULL, hls_worker,
                                   &factory->workers[i]);
            if (r == 0) {
                factory->workers[i].started = 1;
            } else if (i == 0) {
                ret2 = r; /* no workers at all: nothing can ever be served */
            } else {
                vt_log("hls: second worker failed (%d), one connection", r);
            }
        }
        if (attr_ready) pthread_attr_destroy(&attr);
        if (ret2 != 0) {
            pthread_cond_destroy(&factory->worker_wake);
            pthread_mutex_destroy(&factory->worker_lock);
        }
    }
    if (ret2 != 0) {
        vt_log("hls: worker start failed (%d)", ret2);
        vt_hls_stream_factory_term(factory);
        return -1;
    }
    factory->worker_started = 1;

    return 0;
}

/* libavformat seeks an HLS playlist by segment, and it treats the two media
 * types differently: a backward seek on a VIDEO stream is snapped to the start
 * of the segment that contains the target (hls_read_seek), while an audio
 * stream is positioned at the target itself. Resuming a quality switch at, say,
 * 2182 ms therefore restarts video at the segment boundary before it while
 * audio carries on at 2182 ms — the decoder then races to catch video up at
 * maximum speed, which is exactly the state vita_hw_decoder.c's approximate
 * resume exists to avoid. It avoids it by aligning audio to the video keyframe
 * from the container's sample index, and an HLS playlist has no such index.
 *
 * The playlist's own EXTINF list is the very table hls_read_seek adds up, so
 * the segment boundary is computed from it here and handed to BOTH cursors as
 * the resume position. The conversion mirrors libavformat's parse_playlist
 * (atof scaled to microseconds, truncated per segment) so the two arrive at
 * the same segment rather than at neighbouring ones. */
uint64_t vt_hls_segment_start_ms(const VtHlsStreamFactory *factory,
                                 uint64_t position_ms) {
    if (!factory) return position_ms;
    /* The playlist pointer and the live flag are both rewritten by a worker's
     * reload; the const in the signature describes the caller's intent, not
     * freedom from the lock. The walk below only ever runs for a complete
     * playlist — those are never reloaded — but the flag saying so has to be
     * read safely first. */
    VtHlsStreamFactory *f = (VtHlsStreamFactory *)factory;
    if (f->worker_started) pthread_mutex_lock(&f->worker_lock);
    uint64_t result = position_ms;
    /* A live window has no fixed origin — its first entry is whichever
     * segment has not scrolled off yet — so adding up EXTINFs from the top
     * says nothing about where a position sits. Live playback does not seek;
     * this only ever gets asked as part of the shared open path. */
    if (f->playlist && !f->live) {
        int64_t target_us = (int64_t)position_ms * 1000;
        int64_t start_us = 0;
        for (const char *p = f->playlist->text; p; p = next_line(p)) {
            if (strncmp(p, "#EXTINF:", 8) != 0) continue;
            int64_t duration_us = (int64_t)(strtod(p + 8, NULL) * 1000000.0);
            if (duration_us <= 0) continue;
            if (start_us + duration_us > target_us) {
                /* Rounded UP to the millisecond the caller works in: a result
                 * that fell even one microsecond short of this boundary would
                 * put the video cursor in the PREVIOUS segment while the audio
                 * cursor, which skips forward to the timestamp instead, stayed
                 * in this one — a full segment of desync instead of none. */
                result = (uint64_t)((start_us + 999) / 1000);
                break;
            }
            start_us += duration_us;
        }
        /* Past the last segment: leave position_ms to the demuxer. */
    }
    if (f->worker_started) pthread_mutex_unlock(&f->worker_lock);
    return result;
}

void vt_hls_stream_factory_term(VtHlsStreamFactory *factory) {
    if (!factory) return;
    if (factory->worker_started) {
        pthread_mutex_lock(&factory->worker_lock);
        factory->worker_stop = 1;
        pthread_cond_broadcast(&factory->worker_wake);
        pthread_mutex_unlock(&factory->worker_lock);
        /* An in-flight fetch ends via cancel_flag (the decoder is already
         * joined by the time a source is torn down) or its own timeout. */
        for (int i = 0; i < 2; i++) {
            if (!factory->workers[i].started) continue;
            pthread_join(factory->workers[i].thread, NULL);
            factory->workers[i].started = 0;
        }
        pthread_cond_destroy(&factory->worker_wake);
        pthread_mutex_destroy(&factory->worker_lock);
        factory->worker_started = 0;
        factory->worker_stop = 0;
    }
    for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++)
        slot_clear(&factory->slots[i]);
    free(factory->want_url);
    factory->want_url = NULL;
    free(factory->next_url);
    factory->next_url = NULL;
    free(factory->last_url);
    factory->last_url = NULL;
    free(factory->abandoned_url);
    factory->abandoned_url = NULL;
    factory->want_playlist = 0;
    if (factory->client) vita_https_client_destroy(factory->client);
    factory->client = NULL;
    if (factory->client2) vita_https_client_destroy(factory->client2);
    factory->client2 = NULL;
    playlist_release(factory->playlist);
    factory->playlist = NULL;
    playlist_release(factory->session_playlist);
    factory->session_playlist = NULL;
    factory->open_cursors = 0;
}
