/* ViTube — stateless row/empty-state rendering for list screens. */
#ifndef VITUBE_UI_ROWS_VIEW_H
#define VITUBE_UI_ROWS_VIEW_H

#include "yt/model.h"

void draw_video_row(const yt_video_t *video, int y, int selected);
void draw_channel_row(const yt_channel_t *channel, int y, int selected);
/* The startup landing screen shown when the home feed has no content yet. */
void draw_empty_home(int list_top_y, int logged_in);

#endif
