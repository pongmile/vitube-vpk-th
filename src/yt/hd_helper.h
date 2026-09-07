/* ViTube — persisted HD quality preference (local only).
 *
 * The server-side 720p helper this file once fronted is gone: the app now
 * resolves 720p itself via YouTube's HLS (see yt_hls_manifest). All that
 * remains here is the one-byte on/off flag remembering whether the user wants
 * HD, stored in ux0:data/ViTube/hd.flag.
 *
 * The Mehr tab is the only writer. The player reads it once when opening a
 * video and then tracks that video's quality on its own — a pick in the
 * Triangle menu must not reach back here, or choosing 360p for one clip would
 * quietly become the setting for every clip. */
#ifndef VITUBE_YT_HD_HELPER_H
#define VITUBE_YT_HD_HELPER_H

int yt_hd_pref_get(void);
void yt_hd_pref_set(int enabled);

#endif
