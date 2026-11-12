/* ViTube — async image cache (video thumbnails, channel avatars). */
#ifndef VITUBE_UI_THUMBS_H
#define VITUBE_UI_THUMBS_H

#include <vita2d.h>

int vt_thumbs_init(void);
void vt_thumbs_term(void);

/* Non-blocking: returns the texture for `url` (cached under `key`, e.g. a
 * video or channel id) when ready, NULL otherwise (and queues the
 * download). Textures stay owned by the cache. */
vita2d_texture *vt_thumbs_get(const char *key, const char *url);

/* The same for a JPEG that is already a file on the card (the thumbnail saved
 * with a download): no network involved, so it works offline. `key` is the
 * video id; it is kept apart from vt_thumbs_get()'s entries for the same id
 * (see thumbs.c). Same slots, same 48-entry cache, same retry budget. */
vita2d_texture *vt_thumbs_get_file(const char *key, const char *path);

/* Main thread, once per frame: uploads finished downloads (max 2/frame). */
void vt_thumbs_pump(void);

/* Drops every cached image without stopping the worker. Used on logout, so
 * the previous account's channel avatars do not survive in memory. */
void vt_thumbs_clear(void);

#endif
