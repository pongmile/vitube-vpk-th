/* ViTube — fullscreen video player. */
#ifndef VITUBE_UI_PLAYER_SCREEN_H
#define VITUBE_UI_PLAYER_SCREEN_H

#include "yt/model.h"

/* Resolves the stream and plays it. Returns when playback ends. */
void player_screen_run(const yt_video_t *video);

#endif
