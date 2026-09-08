/* ViTube — InnerTube response parsing (pure C + jansson, host-testable). */
#include "parse.h"

#include <jansson.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest prefix of s that fits in n bytes without cutting a UTF-8
 * sequence: if the first excluded byte is a continuation byte, the sequence
 * it belongs to would be cut, so back off to that sequence's lead byte. A
 * sequence that ends exactly at n is kept whole. Titles cut mid-character
 * were not just a stray glyph: json_string() in store.c rejects invalid
 * UTF-8, so the history entry silently vanished. */
static size_t utf8_fit(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len <= n) return len;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = utf8_fit(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* A continuation token is only useful whole. The home feed hands out tokens
 * of 4 KB and more — well past YT_CONTINUATION_MAX — and copy_str would store
 * a truncated one that looks valid to every caller and is rejected by the
 * API. Storing nothing instead makes an empty continuation mean exactly
 * "cannot load more", which is what the callers already test for. */
static void set_continuation(char *dst, size_t cap, const char *token) {
    if (!token || strlen(token) >= cap) return;
    snprintf(dst, cap, "%s", token);
}

/* "text" objects come as {"runs":[{"text":..}, ..]} or {"simpleText":..};
 * runs are concatenated. */
static void copy_text(char *dst, size_t cap, json_t *node) {
    dst[0] = '\0';
    if (!node) return;
    const char *simple = json_string_value(json_object_get(node, "simpleText"));
    if (simple) { copy_str(dst, cap, simple); return; }
    json_t *runs = json_object_get(node, "runs");
    size_t i, used = 0;
    json_t *run;
    json_array_foreach(runs, i, run) {
        const char *t = json_string_value(json_object_get(run, "text"));
        if (!t) continue;
        size_t n = strlen(t);
        int clamped = 0;
        if (used + n >= cap) {
            n = utf8_fit(t, cap - 1 - used);
            clamped = 1;
        }
        memcpy(dst + used, t, n);
        used += n;
        /* Once a run was clamped nothing may follow it, even if the boundary
         * back-off left a few bytes free. */
        if (clamped || used >= cap - 1) break;
    }
    dst[used] = '\0';
}

/* Pick a thumbnail close to 320px wide (Vita list rows are small). */
static void pick_thumb(char *dst, size_t cap, json_t *thumbnail) {
    dst[0] = '\0';
    json_t *thumbs = json_object_get(thumbnail, "thumbnails");
    /* Newer InnerTube viewModel nodes spell the same array "sources". */
    if (!thumbs) thumbs = json_object_get(thumbnail, "sources");
    size_t i;
    json_t *t;
    int best_delta = 1 << 30;
    json_array_foreach(thumbs, i, t) {
        int w = (int)json_integer_value(json_object_get(t, "width"));
        const char *url = json_string_value(json_object_get(t, "url"));
        if (!url || strlen(url) >= cap) continue; /* would silently truncate */
        int delta = w > 320 ? w - 320 : 320 - w;
        if (delta < best_delta) {
            best_delta = delta;
            copy_str(dst, cap, url);
        }
    }
}

/* First image URL anywhere in a subtree: any node carrying a "thumbnails" or
 * "sources" array. Channel tiles put the avatar under a key that varies with
 * the TV client version, so the specific lookup in fill_channel_tile() gets
 * this as a fallback instead of silently yielding no avatar at all — which is
 * exactly how every channel row came to draw an empty square (2026-08-28). */
static void find_any_thumb(char *dst, size_t cap, json_t *node) {
    if (json_is_object(node)) {
        if (json_object_get(node, "thumbnails") ||
            json_object_get(node, "sources")) {
            pick_thumb(dst, cap, node);
            if (dst[0]) return;
        }
        const char *key;
        json_t *value;
        json_object_foreach(node, key, value) {
            find_any_thumb(dst, cap, value);
            if (dst[0]) return;
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) {
            find_any_thumb(dst, cap, value);
            if (dst[0]) return;
        }
    }
}

static void duration_from_overlays(char *dst, size_t cap, json_t *renderer) {
    dst[0] = '\0';
    json_t *overlays = json_object_get(renderer, "thumbnailOverlays");
    size_t i;
    json_t *ov;
    json_array_foreach(overlays, i, ov) {
        json_t *ts = json_object_get(ov, "thumbnailOverlayTimeStatusRenderer");
        if (!ts) continue;
        copy_text(dst, cap, json_object_get(ts, "text"));
        if (dst[0]) return;
    }
}

/* YouTube never returns a publication timestamp for list items — only a
 * localized relative age ("vor 3 Tagen", "3 days ago", "vor 1 Monat
 * gestreamt"). Convert the first "<number> <unit>" pair in `s` to minutes so
 * lists can be ordered newest-first. Leading view counts are skipped for
 * free: in "6.167 Aufrufe · vor 1 Monat" neither "6." nor "167 Aufrufe" is
 * followed by a unit word, so the scan walks on to "1 Monat". Returns -1 when
 * nothing matched (live streams: "12.345 Zuschauer", "LIVE"). */
static int age_minutes(const char *s) {
    /* Prefix stems, matched case-sensitively against the word after the
     * number, so every inflection matches ("Tagen", "days", "giorni"). The
     * loop takes the first match: a short stem must never precede a longer
     * one with a different meaning — the short ones here ("mes", "an",
     * "or") only ever prefix words of the same unit ("mese"/"meses",
     * "anno"/"ans", "ora"/"ore"). Every hl region.c offers has to be here,
     * or newest-first sorting silently becomes a no-op for that region. */
    static const struct { const char *word; int minutes; } units[] = {
        { "Sekunde", 0 },      { "second", 0 },
        { "Minute",  1 },      { "minute", 1 },
        { "Stunde",  60 },     { "hour",   60 },
        { "Tag",     1440 },   { "day",    1440 },
        { "Woche",   10080 },  { "week",   10080 },
        { "Monat",   43200 },  { "month",  43200 },
        { "Jahr",    525600 }, { "year",   525600 },
        /* fr: il y a 3 secondes / minutes / heures / jours / semaines /
         * mois / 2 ans / 1 an */
        { "seconde", 0 },      { "heure",  60 },     { "jour", 1440 },
        { "semaine", 10080 },  { "mois",   43200 },
        /* es: hace 3 segundos / minutos / horas / días / semanas / meses /
         * años */
        { "segundo", 0 },      { "minuto", 1 },      { "hora", 60 },
        { "d\xc3\xad" "a", 1440 } /* día; split: \xada would be one escape */, { "semana", 10080 },  { "mes",  43200 },
        { "a\xc3\xb1o", 525600 },
        /* it: 3 secondi / minuti / ore / giorni / settimane / mesi / anni fa */
        { "secondi", 0 },      { "minuti", 1 },      { "or",   60 },
        { "giorn",   1440 },   { "settiman", 10080 }, { "ann",  525600 },
        { "an",      525600 },
        /* pt (Brazil): há 3 segundos / minutos / horas / dias / semanas /
         * 1 mês / 5 meses / anos. segundo, minuto, hora, semana and mes are
         * the Spanish stems above; "an" covers ano/anos. Only the two words
         * Spanish spells differently are new. */
        { "dia",     1440 },   { "m\xc3\xaas", 43200 },
    };
    if (!s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        long n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        const char *w = p;
        for (;;) { /* skip thousands/decimal separators, spaces and NBSP */
            if (*w == ' ' || *w == '.' || *w == ',') { w++; continue; }
            if ((unsigned char)w[0] == 0xc2 && (unsigned char)w[1] == 0xa0) {
                w += 2;
                continue;
            }
            break;
        }
        for (size_t u = 0; u < sizeof units / sizeof *units; u++) {
            if (strncmp(w, units[u].word, strlen(units[u].word)) != 0) continue;
            long total = n * units[u].minutes;
            return total > INT_MAX ? INT_MAX : (int)total;
        }
        p--; /* the loop's p++ must not skip the char that ended the number */
    }
    return -1;
}

static const char *find_browse_channel_id(json_t *node); /* defined below */

static void fill_video(yt_video_t *v, json_t *r) {
    copy_str(v->video_id, sizeof v->video_id,
             json_string_value(json_object_get(r, "videoId")));
    json_t *title = json_object_get(r, "title");
    if (!title) title = json_object_get(r, "headline");
    copy_text(v->title, sizeof v->title, title);
    copy_text(v->channel, sizeof v->channel, json_object_get(r, "shortBylineText"));
    if (!v->channel[0])
        copy_text(v->channel, sizeof v->channel, json_object_get(r, "longBylineText"));
    copy_text(v->views, sizeof v->views, json_object_get(r, "shortViewCountText"));
    if (!v->views[0])
        copy_text(v->views, sizeof v->views, json_object_get(r, "viewCountText"));
    copy_text(v->duration, sizeof v->duration, json_object_get(r, "lengthText"));
    if (!v->duration[0])
        duration_from_overlays(v->duration, sizeof v->duration, r);
    pick_thumb(v->thumb_url, sizeof v->thumb_url, json_object_get(r, "thumbnail"));
    char published[YT_META_MAX];
    copy_text(published, sizeof published,
              json_object_get(r, "publishedTimeText"));
    v->age_min = age_minutes(published[0] ? published : v->views);
    copy_str(v->channel_id, sizeof v->channel_id, find_browse_channel_id(r));
}

/* Find the first "watchEndpoint"."videoId" inside a subtree. */
static const char *find_watch_video_id(json_t *node) {
    if (json_is_object(node)) {
        json_t *we = json_object_get(node, "watchEndpoint");
        if (we) {
            const char *id = json_string_value(json_object_get(we, "videoId"));
            if (id) return id;
        }
        const char *key;
        json_t *value;
        json_object_foreach(node, key, value) {
            const char *id = find_watch_video_id(value);
            if (id) return id;
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) {
            const char *id = find_watch_video_id(value);
            if (id) return id;
        }
    }
    return NULL;
}

/* Find the first "browseEndpoint"."browseId" that looks like a channel id
 * (starts with "UC") inside a subtree — used to resolve channel tiles. */
static const char *find_browse_channel_id(json_t *node) {
    if (json_is_object(node)) {
        json_t *be = json_object_get(node, "browseEndpoint");
        if (be) {
            const char *id = json_string_value(json_object_get(be, "browseId"));
            if (id && strncmp(id, "UC", 2) == 0) return id;
        }
        const char *key;
        json_t *value;
        json_object_foreach(node, key, value) {
            const char *id = find_browse_channel_id(value);
            if (id) return id;
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) {
            const char *id = find_browse_channel_id(value);
            if (id) return id;
        }
    }
    return NULL;
}

/* Depth-first search for the first object/array value stored under `key`,
 * anywhere in the tree. Used for one-off metadata that doesn't live at a
 * fixed, predictable depth (e.g. a channel page's title). */
static json_t *find_key(json_t *node, const char *key) {
    if (json_is_object(node)) {
        json_t *found = json_object_get(node, key);
        if (found) return found;
        const char *k;
        json_t *value;
        json_object_foreach(node, k, value) {
            found = find_key(value, key);
            if (found) return found;
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) {
            json_t *found = find_key(value, key);
            if (found) return found;
        }
    }
    return NULL;
}

/* Android "element" item: videoWithContextData (used by /next related list). */
static void fill_video_with_context(yt_video_t *v, json_t *data, json_t *item_root) {
    json_t *vd = json_object_get(data, "videoData");
    json_t *lm = json_object_get(json_object_get(vd, "lockupMetadata"),
                                 "lockupMetadataViewModel");
    copy_str(v->title, sizeof v->title,
             json_string_value(json_object_get(json_object_get(lm, "title"), "content")));
    copy_str(v->duration, sizeof v->duration,
             json_string_value(json_object_get(json_object_get(vd, "thumbnail"),
                                               "timestampText")));
    /* metadata row: "Channel · 6,8 Mio. Aufrufe · vor 8 Monaten" */
    json_t *mrows = json_object_get(json_object_get(json_object_get(lm, "metadata"),
                                    "contentMetadataViewModel"), "metadataRows");
    size_t i;
    json_t *row;
    json_array_foreach(mrows, i, row) {
        json_t *parts = json_object_get(row, "metadataParts");
        if (!json_array_size(parts)) continue;
        const char *text = json_string_value(json_object_get(
            json_object_get(json_array_get(parts, 0), "text"), "content"));
        if (!text || !text[0]) continue;
        const char *dot = strstr(text, "\xc2\xb7"); /* "·" */
        if (dot && dot > text) {
            size_t n = (size_t)(dot - text);
            while (n && text[n - 1] == ' ') n--;
            if (n >= sizeof v->channel)
                n = utf8_fit(text, sizeof v->channel - 1);
            memcpy(v->channel, text, n);
            v->channel[n] = '\0';
            const char *rest = dot + 2;
            while (*rest == ' ') rest++;
            copy_str(v->views, sizeof v->views, rest);
        } else {
            copy_str(v->channel, sizeof v->channel, text);
        }
        break;
    }
    v->age_min = age_minutes(v->views); /* set before both exits below */
    const char *id = find_watch_video_id(item_root);
    if (!id) { /* fall back to dragAndDropUrl */
        const char *url = json_string_value(json_object_get(vd, "dragAndDropUrl"));
        const char *p = url ? strstr(url, "v=") : NULL;
        if (p && strlen(p + 2) >= 11) {
            memcpy(v->video_id, p + 2, 11);
            v->video_id[11] = '\0';
        }
        return;
    }
    copy_str(v->video_id, sizeof v->video_id, id);
}

/* TVHTML5 item: tileRenderer (used by the TV UI's subscriptions feed). */
static void fill_tile(yt_video_t *v, json_t *tile) {
    const char *type = json_string_value(json_object_get(tile, "contentType"));
    if (type && strcmp(type, "TILE_CONTENT_TYPE_VIDEO") != 0) return;
    copy_str(v->video_id, sizeof v->video_id, find_watch_video_id(tile));
    json_t *md = json_object_get(json_object_get(tile, "metadata"),
                                 "tileMetadataRenderer");
    copy_text(v->title, sizeof v->title, json_object_get(md, "title"));
    /* lines: usually [channel], [views · age] — items concatenated */
    json_t *lines = json_object_get(md, "lines");
    size_t li;
    json_t *line;
    json_array_foreach(lines, li, line) {
        char *dst = li == 0 ? v->channel : v->views;
        size_t cap = li == 0 ? sizeof v->channel : sizeof v->views;
        size_t used = 0;
        json_t *items = json_object_get(json_object_get(line, "lineRenderer"),
                                        "items");
        size_t ii;
        json_t *item;
        json_array_foreach(items, ii, item) {
            char part[96];
            copy_text(part, sizeof part,
                      json_object_get(json_object_get(item, "lineItemRenderer"),
                                      "text"));
            if (!part[0]) continue;
            size_t n = strlen(part);
            if (used && used + 1 < cap - 1) dst[used++] = ' ';
            if (n > cap - 1 - used) n = utf8_fit(part, cap - 1 - used);
            memcpy(dst + used, part, n);
            used += n;
        }
        dst[used] = '\0';
        if (li >= 1) break;
    }
    json_t *header = json_object_get(json_object_get(tile, "header"),
                                     "tileHeaderRenderer");
    duration_from_overlays(v->duration, sizeof v->duration, header);
    pick_thumb(v->thumb_url, sizeof v->thumb_url,
              json_object_get(header, "thumbnail"));
    v->age_min = age_minutes(v->views);
    copy_str(v->channel_id, sizeof v->channel_id, find_browse_channel_id(tile));
}

/* TVHTML5 item: tileRenderer with contentType CHANNEL (subscribed-channels
 * list). Mirrors fill_tile()'s video case but for a channel target. */
static void fill_channel_tile(yt_channel_t *c, json_t *tile) {
    const char *type = json_string_value(json_object_get(tile, "contentType"));
    if (type && strcmp(type, "TILE_CONTENT_TYPE_CHANNEL") != 0) return;
    copy_str(c->channel_id, sizeof c->channel_id, find_browse_channel_id(tile));
    json_t *md = json_object_get(json_object_get(tile, "metadata"),
                                 "tileMetadataRenderer");
    copy_text(c->title, sizeof c->title, json_object_get(md, "title"));
    json_t *header = json_object_get(json_object_get(tile, "header"),
                                     "tileHeaderRenderer");
    pick_thumb(c->avatar_url, sizeof c->avatar_url,
              json_object_get(header, "thumbnail"));
    /* That one path was written by analogy to the video tile and never held
     * for real channel tiles, so every avatar came back empty. Fall back to
     * any image in the tile rather than showing a blank square. */
    if (!c->avatar_url[0])
        find_any_thumb(c->avatar_url, sizeof c->avatar_url, tile);
}

/* ANDROID search item: the Elements "compact_channel" component. Client 21.x
 * no longer emits channelRenderer at all — a channel result arrives as
 * elementRenderer > newElement > type > componentType > model >
 * compactChannelModel > compactChannelData (verified live 2026-08-28 against
 * queries "Lofi Girl", "podcast", "minecraft").
 *
 * Two shape traps, both hit in real responses:
 *  - the display name is a PLAIN STRING under "title", except on badged
 *    channels (verified artist), where "title" is absent and the name lives
 *    in "attributedTitle"."content". Neither is a runs/simpleText object, so
 *    copy_text() must not be used here.
 *  - the avatar array is spelled "sources", not "thumbnails", and mixes in
 *    url-less clientResource entries. pick_thumb() already handles both. */
static void fill_search_channel(yt_channel_t *c, json_t *data) {
    copy_str(c->channel_id, sizeof c->channel_id, find_browse_channel_id(data));
    const char *name = json_string_value(json_object_get(data, "title"));
    if (!name)
        name = json_string_value(json_object_get(
            json_object_get(data, "attributedTitle"), "content"));
    copy_str(c->title, sizeof c->title, name);
    pick_thumb(c->avatar_url, sizeof c->avatar_url,
               json_object_get(json_object_get(data, "avatar"), "image"));
    if (!c->avatar_url[0])
        find_any_thumb(c->avatar_url, sizeof c->avatar_url, data);
}

/* Depth-first walk collecting channel tiles and the first continuation
 * token. Mirrors walk() below but targets yt_channel_list_t. */
static void walk_channels(json_t *node, yt_channel_list_t *out) {
    if (json_is_object(node)) {
        const char *key;
        json_t *value;
        json_object_foreach(node, key, value) {
            if (out->count < YT_LIST_MAX && strcmp(key, "tileRenderer") == 0) {
                yt_channel_t *c = &out->items[out->count];
                memset(c, 0, sizeof *c);
                fill_channel_tile(c, value);
                if (c->channel_id[0] && c->title[0]) out->count++;
                continue;
            }
            if (!out->continuation[0]) {
                if (strcmp(key, "nextContinuationData") == 0) {
                    set_continuation(out->continuation,
                                     sizeof out->continuation,
                                     json_string_value(json_object_get(
                                         value, "continuation")));
                } else if (strcmp(key, "continuationCommand") == 0) {
                    set_continuation(out->continuation,
                                     sizeof out->continuation,
                                     json_string_value(json_object_get(
                                         value, "token")));
                }
            }
            walk_channels(value, out);
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) walk_channels(value, out);
    }
}

/* Depth-first walk collecting known video renderers and the first
 * continuation token, in document order. `chans` is optional: pass it to
 * also collect the channel results a search response carries, so one parsed
 * root yields both lists — re-parsing a ~1 MB body a second time would
 * double peak heap and parse time on the Vita for no gain. Pass NULL for
 * every non-search surface; behaviour is then unchanged. */
static void walk(json_t *node, yt_video_list_t *out, yt_channel_list_t *chans) {
    if (json_is_object(node)) {
        const char *key;
        json_t *value;
        json_object_foreach(node, key, value) {
            if (chans && chans->count < YT_LIST_MAX &&
                strcmp(key, "compactChannelData") == 0) {
                yt_channel_t *c = &chans->items[chans->count];
                memset(c, 0, sizeof *c);
                fill_search_channel(c, value);
                if (c->channel_id[0] && c->title[0]) chans->count++;
                continue; /* no videos nested inside a channel result */
            }
            if (out->count < YT_LIST_MAX && strcmp(key, "videoWithContextData") == 0) {
                yt_video_t *v = &out->items[out->count];
                memset(v, 0, sizeof *v);
                fill_video_with_context(v, value, node);
                if (v->video_id[0] && v->title[0]) out->count++;
                continue;
            }
            if (out->count < YT_LIST_MAX && strcmp(key, "tileRenderer") == 0) {
                yt_video_t *v = &out->items[out->count];
                memset(v, 0, sizeof *v);
                fill_tile(v, value);
                if (v->video_id[0] && v->title[0]) out->count++;
                continue;
            }
            if (out->count < YT_LIST_MAX &&
                (strcmp(key, "compactVideoRenderer") == 0 ||
                 strcmp(key, "videoWithContextRenderer") == 0 ||
                 strcmp(key, "videoRenderer") == 0 ||
                 strcmp(key, "gridVideoRenderer") == 0 ||
                 strcmp(key, "reelItemRenderer") == 0)) {
                yt_video_t *v = &out->items[out->count];
                memset(v, 0, sizeof *v);
                fill_video(v, value);
                if (v->video_id[0] && v->title[0]) out->count++;
                continue; /* no nested videos inside a renderer */
            }
            if (!out->continuation[0]) {
                if (strcmp(key, "nextContinuationData") == 0) {
                    set_continuation(out->continuation,
                                     sizeof out->continuation,
                                     json_string_value(json_object_get(
                                         value, "continuation")));
                } else if (strcmp(key, "continuationCommand") == 0) {
                    set_continuation(out->continuation,
                                     sizeof out->continuation,
                                     json_string_value(json_object_get(
                                         value, "token")));
                }
            }
            walk(value, out, chans);
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t *value;
        json_array_foreach(node, i, value) walk(value, out, chans);
    }
}

void yt_sort_newest_first(yt_video_list_t *list) {
    /* Insertion sort, kept stable on purpose: the relative age is coarse
     * ("vor 1 Monat" spans a month), so items with an equal key must keep the
     * finer order YouTube returned them in. Unknown ages (-1) sink to the
     * end, also in their original order. */
    for (int a = 1; a < list->count; a++) {
        yt_video_t tmp = list->items[a];
        int key = tmp.age_min < 0 ? INT_MAX : tmp.age_min;
        int b = a - 1;
        while (b >= 0) {
            int other = list->items[b].age_min < 0 ? INT_MAX
                                                   : list->items[b].age_min;
            if (other <= key) break;
            list->items[b + 1] = list->items[b];
            b--;
        }
        list->items[b + 1] = tmp;
    }
}

int yt_parse_video_list(const char *json, size_t len, yt_video_list_t *out) {
    memset(out, 0, sizeof *out);
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;
    walk(root, out, NULL);
    json_decref(root);
    return out->count;
}

int yt_parse_search(const char *json, size_t len, yt_video_list_t *out,
                    yt_channel_list_t *out_channels) {
    memset(out, 0, sizeof *out);
    if (out_channels) memset(out_channels, 0, sizeof *out_channels);
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;
    walk(root, out, out_channels);
    /* out_channels->continuation is deliberately left empty: a search page
     * carries one continuation token for the whole page (already stored in
     * out->continuation) and continuing it returns videos, not channels. */
    json_decref(root);
    return out->count;
}

int yt_parse_channel_list(const char *json, size_t len, yt_channel_list_t *out) {
    memset(out, 0, sizeof *out);
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;
    walk_channels(root, out);
    json_decref(root);
    return out->count;
}

int yt_parse_channel_page(const char *json, size_t len, yt_video_list_t *out,
                          char *out_title, size_t title_cap) {
    memset(out, 0, sizeof *out);
    if (out_title && title_cap) out_title[0] = '\0';
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;
    walk(root, out, NULL);
    if (out_title && title_cap) {
        json_t *header = find_key(root, "channelHeaderRenderer");
        copy_text(out_title, title_cap, json_object_get(header, "title"));
    }
    json_decref(root);
    return out->count;
}

int yt_parse_player(const char *json, size_t len, yt_stream_t *out) {
    memset(out, 0, sizeof *out);
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;

    json_t *ps = json_object_get(root, "playabilityStatus");
    copy_str(out->playability, sizeof out->playability,
             json_string_value(json_object_get(ps, "status")));
    copy_text(out->reason, sizeof out->reason, json_object_get(ps, "reason"));
    if (!out->reason[0])
        copy_str(out->reason, sizeof out->reason,
                 json_string_value(json_object_get(ps, "reason")));

    json_t *vd = json_object_get(root, "videoDetails");
    copy_str(out->video_id, sizeof out->video_id,
             json_string_value(json_object_get(vd, "videoId")));
    copy_str(out->title, sizeof out->title,
             json_string_value(json_object_get(vd, "title")));
    copy_str(out->channel, sizeof out->channel,
             json_string_value(json_object_get(vd, "author")));
    const char *lensec = json_string_value(json_object_get(vd, "lengthSeconds"));
    if (lensec) out->length_seconds = atoi(lensec);
    out->is_live = json_is_true(json_object_get(vd, "isLive"));

    /* Progressive (muxed video+audio) formats: streamingData.formats[].
     * Collect every usable one instead of assuming itag 18 — this stays
     * correct if YouTube ever serves a higher-res progressive stream for a
     * given video again. Ciphered formats (no plain "url") are skipped: we
     * have no JS runtime on the Vita to run YouTube's signature descrambler. */
    json_t *sd = json_object_get(root, "streamingData");
    /* A live stream never has one of those, and its HLS manifest is the only
     * thing that can be played — so it is taken from THIS response rather
     * than resolved again: this client's live manifest is the muxed,
     * short-window one that hls.c reloads, and it is already here. */
    if (out->is_live)
        copy_str(out->hls_manifest, sizeof out->hls_manifest,
                 json_string_value(json_object_get(sd, "hlsManifestUrl")));
    json_t *formats = json_object_get(sd, "formats");
    size_t i;
    json_t *f;
    json_array_foreach(formats, i, f) {
        if (out->format_count >= YT_FORMATS_MAX) break;
        const char *url = json_string_value(json_object_get(f, "url"));
        if (!url) continue;
        const char *mime = json_string_value(json_object_get(f, "mimeType"));
        if (mime && strncmp(mime, "video/mp4", 9) != 0) continue;
        yt_format_t *fmt = &out->formats[out->format_count];
        fmt->itag = (int)json_integer_value(json_object_get(f, "itag"));
        fmt->width = (int)json_integer_value(json_object_get(f, "width"));
        fmt->height = (int)json_integer_value(json_object_get(f, "height"));
        fmt->fps = (int)json_integer_value(json_object_get(f, "fps"));
        const char *cl = json_string_value(json_object_get(f, "contentLength"));
        fmt->content_length = cl ? atoll(cl) : 0;
        copy_str(fmt->url, sizeof fmt->url, url);
        out->format_count++;
    }
    /* Sort descending by height so formats[0] is the best default quality. */
    for (int a = 1; a < out->format_count; a++) {
        yt_format_t tmp = out->formats[a];
        int b = a - 1;
        while (b >= 0 && out->formats[b].height < tmp.height) {
            out->formats[b + 1] = out->formats[b];
            b--;
        }
        out->formats[b + 1] = tmp;
    }
    out->selected = 0;
    json_decref(root);
    return 0;
}
