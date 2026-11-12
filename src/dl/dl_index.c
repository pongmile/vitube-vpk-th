/* ViTube — index.json: the list of downloads on the card; see dl_core.h.
 *
 * The file is untrusted input. The memory card can hand back a truncated or
 * overwritten file after a crash, and the directory is world-readable to
 * anything with VitaShell: so every id is checked before it can become a file
 * name, every string is bounded and re-encoded, every number is clamped, and
 * a file that is not an index at all yields "no entries" rather than a
 * crash. */
#include "dl/dl_core.h"

#include <stdlib.h>
#include <string.h>

#include <jansson.h>

/* Numbers in the file are sizes of videos: anything past this is damage. */
#define INDEX_INT_MAX ((int64_t)1 << 42)

int dl_valid_id(const char *id) {
    if (!id) return 0;
    size_t n = 0;
    for (; id[n]; n++) {
        if (n >= YT_VIDEO_ID_LEN - 1) return 0; /* too long: stop reading */
        char ch = id[n];
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) return 0;
    }
    return n == YT_VIDEO_ID_LEN - 1;
}

/* Length of the well-formed UTF-8 sequence at s (1..4), or 0. The same
 * strictness as jansson's own check (no overlong forms, no surrogates, nothing
 * above U+10FFFF), because that is the parser index.json must pass. */
static int utf8_len(const unsigned char *s) {
    unsigned char a = s[0];
    if (a < 0x80) return 1;
    if (a >= 0xC2 && a <= 0xDF)
        return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (a >= 0xE0 && a <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (a == 0xE0 && s[1] < 0xA0) return 0;  /* overlong */
        if (a == 0xED && s[1] > 0x9F) return 0;  /* surrogate */
        return 3;
    }
    if (a >= 0xF0 && a <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            return 0;
        if (a == 0xF0 && s[1] < 0x90) return 0;  /* overlong */
        if (a == 0xF4 && s[1] > 0x8F) return 0;  /* above U+10FFFF */
        return 4;
    }
    return 0;
}

void dl_clean_copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t out = 0;
    if (src) {
        const unsigned char *s = (const unsigned char *)src;
        while (*s) {
            /* utf8_len reads up to three bytes past s[0]; they are only
             * looked at when the lead byte promises them, and a NUL in any
             * continuation position fails the (b & 0xC0) == 0x80 test
             * before the next one is read. */
            int n = utf8_len(s);
            if (n == 0) {
                if (out + 1 >= cap) break;
                dst[out++] = '?';
                s++;
                continue;
            }
            if (n == 1 && (*s < 0x20 || *s == 0x7F)) {
                if (out + 1 >= cap) break;
                dst[out++] = ' ';
                s++;
                continue;
            }
            if (out + (size_t)n >= cap) break;
            memcpy(dst + out, s, (size_t)n);
            out += (size_t)n;
            s += n;
        }
    }
    dst[out] = '\0';
}

static const char *state_key(DlState s) {
    switch (s) {
    case DL_PAUSED: return "paused";
    case DL_FAILED: return "failed";
    case DL_DONE: return "done";
    default:
        /* Resolving, downloading, waiting, verifying: what they have in
         * common is that the app closing interrupted them. They all start
         * again as "queued". */
        return "queued";
    }
}

char *dl_index_serialize(const DlCore *c, size_t *len) {
    json_t *root = json_object();
    json_t *items = json_array();
    if (!root || !items) {
        json_decref(root);
        json_decref(items);
        return NULL;
    }
    for (int i = 0; i < c->count; i++) {
        const DlCoreItem *it = &c->items[i];
        if (it->pending_delete) continue;
        json_t *o = json_pack(
            "{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i,s:I,s:I,s:i}",
            "id", it->pub.video_id, "title", it->pub.title,
            "channel", it->pub.channel, "duration", it->pub.duration,
            "thumb_url", it->thumb_url, "state", state_key(it->pub.state),
            "error", dl_error_key(it->pub.state == DL_FAILED ? it->pub.error
                                                              : DL_ERR_NONE),
            "height", it->pub.height, "itag", it->itag,
            "done", (json_int_t)it->pub.bytes_done,
            "total", (json_int_t)it->pub.bytes_total,
            "thumb_tries", (int)it->thumb_tries);
        if (!o || json_array_append_new(items, o) < 0) {
            /* json_pack fails on text that is not UTF-8; every string here
             * went through dl_clean_copy, so this is not expected. The
             * entry's files stay on the card, its row is what is lost. */
            json_decref(o);
        }
    }
    json_object_set_new(root, "v", json_integer(1));
    json_object_set_new(root, "items", items);
    char *text = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (text && len) *len = strlen(text);
    return text;
}

static int64_t int_field(json_t *o, const char *key, int64_t lo, int64_t hi) {
    json_t *v = json_object_get(o, key);
    if (!json_is_integer(v)) return lo;
    json_int_t x = json_integer_value(v);
    if (x < lo) return lo;
    if (x > hi) return hi;
    return (int64_t)x;
}

static void string_field(char *dst, size_t cap, json_t *o, const char *key) {
    dl_clean_copy(dst, cap, json_string_value(json_object_get(o, key)));
}

static DlState state_from_key(const char *s) {
    if (s) {
        if (strcmp(s, "paused") == 0) return DL_PAUSED;
        if (strcmp(s, "failed") == 0) return DL_FAILED;
        if (strcmp(s, "done") == 0) return DL_DONE;
    }
    return DL_QUEUED;
}

static DlError error_from_key(const char *s) {
    if (s)
        for (int e = DL_ERR_NONE; e <= DL_ERR_URL; e++)
            if (strcmp(s, dl_error_key((DlError)e)) == 0) return (DlError)e;
    return DL_ERR_IO;
}

int dl_index_parse(DlCore *c, const char *text, size_t len) {
    if (!text || len == 0) return -1;
    json_error_t err;
    json_t *root = json_loadb(text, len, 0, &err);
    if (!root) return -1;
    if (!json_is_object(root)) {
        json_decref(root);
        return -1;
    }
    c->count = 0;
    json_t *items = json_object_get(root, "items");
    size_t i;
    json_t *o;
    json_array_foreach(items, i, o) {
        if (c->count >= DL_MAX_ITEMS) break;
        if (!json_is_object(o)) continue;
        const char *id = json_string_value(json_object_get(o, "id"));
        if (!dl_valid_id(id)) continue;
        int dup = 0;
        for (int k = 0; k < c->count; k++)
            if (strcmp(c->items[k].pub.video_id, id) == 0) { dup = 1; break; }
        if (dup) continue;

        DlCoreItem *it = &c->items[c->count];
        memset(it, 0, sizeof *it);
        memcpy(it->pub.video_id, id, YT_VIDEO_ID_LEN - 1);
        string_field(it->pub.title, sizeof it->pub.title, o, "title");
        if (!it->pub.title[0])
            memcpy(it->pub.title, id, YT_VIDEO_ID_LEN);
        string_field(it->pub.channel, sizeof it->pub.channel, o, "channel");
        string_field(it->pub.duration, sizeof it->pub.duration, o, "duration");
        string_field(it->thumb_url, sizeof it->thumb_url, o, "thumb_url");
        it->pub.state = state_from_key(
            json_string_value(json_object_get(o, "state")));
        it->pub.error = it->pub.state == DL_FAILED
            ? error_from_key(json_string_value(json_object_get(o, "error")))
            : DL_ERR_NONE;
        if (it->pub.state == DL_FAILED && it->pub.error == DL_ERR_NONE)
            it->pub.error = DL_ERR_IO;
        it->pub.height = (int)int_field(o, "height", 0, 100000);
        it->itag = (int)int_field(o, "itag", 0, 1000000);
        it->pub.bytes_done = int_field(o, "done", 0, INDEX_INT_MAX);
        it->pub.bytes_total = int_field(o, "total", 0, INDEX_INT_MAX);
        it->thumb_tries = (unsigned char)int_field(o, "thumb_tries", 0, 255);
        it->uid = ++c->next_uid;
        it->qseq = ++c->next_qseq;
        c->count++;
    }
    json_decref(root);
    return c->count;
}
