/* ViTube — stateless row/empty-state rendering for list screens. */
#ifndef VITUBE_UI_ROWS_VIEW_H
#define VITUBE_UI_ROWS_VIEW_H

#include "yt/model.h"

/* `y` may put the row partly outside the list band [clip_top, clip_bottom)
 * — the lists scroll by the pixel, so the rows at either edge usually are.
 * The caller enables clipping to that band before the first row; these keep
 * it in force (see rows_view.c) and leave it enabled for the next row. */
void draw_video_row(const yt_video_t *video, int y, int selected,
                    int clip_top, int clip_bottom);
void draw_channel_row(const yt_channel_t *channel, int y, int selected,
                      int clip_top, int clip_bottom);
/* The startup landing screen shown when the home feed has no content yet. */
void draw_empty_home(int list_top_y, int logged_in);

#endif
