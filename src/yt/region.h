/* ViTube — the hl/gl pair every InnerTube request carries, and the stored
 * choice behind it (ux0:data/ViTube/region.txt, one digit).
 *
 * These were three hardcoded "de"/"DE" literals inside client_context().
 * They decide the language of every title, duration and relative date the
 * app shows, and which country's results a search returns — not something a
 * user of a German build should have to rebuild to change. */
#ifndef VITUBE_YT_REGION_H
#define VITUBE_YT_REGION_H

typedef struct {
    const char *label; /* shown in the picker */
    const char *hl;    /* interface language, e.g. "de" */
    const char *gl;    /* content country, e.g. "DE" */
} yt_region_t;

int yt_region_count(void);
const yt_region_t *yt_region_at(int index);

/* Reads the stored choice into memory. Call once at startup: every request
 * asks for hl/gl, and those must not touch the memory card. */
void yt_region_load(void);

int yt_region_index(void);
/* Stores and applies immediately. Callers must drop whatever they fetched
 * with the old pair — it is in the wrong language. */
void yt_region_set_index(int index);

const char *yt_region_hl(void);
const char *yt_region_gl(void);

#endif
