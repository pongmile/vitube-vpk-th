/* ViTube — user-facing text in German and English.
 *
 * Every string the app puts on screen goes through vt_str(). Log lines do
 * NOT: they are developer-facing and stay as they are. Neither do the unit
 * words in parse.c's age_minutes() — those read YouTube's own output and have
 * to follow the region's hl, not the interface language.
 *
 * The catalogue is one table of {de, en} pairs indexed by the enum below, so
 * a translation cannot drift out of alignment with its key the way two
 * parallel arrays can. tests/test_strings.c checks that both halves exist and
 * that their printf conversions match. */
#ifndef VITUBE_UI_STRINGS_H
#define VITUBE_UI_STRINGS_H

typedef enum {
    VT_LANG_AUTO = 0, /* follow the region's hl */
    VT_LANG_DE,
    VT_LANG_EN,
    VT_LANG_CHOICE_COUNT
} VtLangChoice;

/* Reads the stored choice. Call once at startup, after yt_region_load():
 * VT_LANG_AUTO resolves against the region. */
void vt_lang_load(void);
VtLangChoice vt_lang_choice(void);
void vt_lang_set_choice(VtLangChoice choice);
const char *vt_lang_label(VtLangChoice choice);
/* What the app is actually speaking right now — "Deutsch" or "English".
 * Shown beside the Automatic entry so that choice is not a promise the user
 * has to go and verify somewhere else. */
const char *vt_lang_effective_label(void);

typedef enum {
    /* --- app-wide ---------------------------------------------------- */
    S_CANCEL_HINT,
    S_CANCELLING,
    S_NO_NETWORK,
    S_CONNECT_WIFI,
    S_NET_INIT_FAILED,

    /* --- tabs and the shell ------------------------------------------ */
    S_TAB_HOME,
    S_TAB_SEARCH,
    S_TAB_SUBS,
    S_TAB_MORE,
    S_SEARCH_PROMPT,
    S_SEARCHING,
    S_SEARCH_FAILED,
    S_CHECK_NETWORK,
    S_LOADING_MORE,
    S_LOADING_HOME,
    S_LOADING_NEW_VIDEOS,
    S_LOADING_CHANNELS,
    S_LOADING_CHANNEL_VIDEOS,
    S_CHANNEL_UNAVAILABLE,
    S_NOT_SIGNED_IN,
    S_SIGN_IN_MORE_TAB,
    S_NO_NEW_VIDEOS,
    S_UNKNOWN_ERROR,
    S_SUBSCRIBING,
    S_UNSUBSCRIBING,
    S_SUBSCRIBE_FAILED,
    S_UNSUBSCRIBE_FAILED,
    S_SUBSCRIBED,
    S_UNSUBSCRIBED,
    S_CHANNELS_ALPHA,
    S_CHANNELS_RECENT,
    S_NEW_VIDEOS,
    S_SUFFIX_CHANNELS,
    S_NO_CHANNELS_FOUND,
    S_NO_SUBSCRIBED_CHANNELS,
    S_SEARCH_EMPTY,
    S_NO_VIDEOS,
    S_RECENTLY_WATCHED,
    S_NOTHING_WATCHED_YET,
    S_RECENT_SEARCHES,
    S_NOTHING_SEARCHED_YET,
    S_HINT_HISTORY,
    S_HINT_SEARCH_RECENT,
    S_HINT_SEARCHES_VIEW,
    S_HINT_SEARCHES_EMPTY,

    /* --- home empty state -------------------------------------------- */
    S_NO_VIDEOS_LOADED,
    S_CHECK_WIFI_RESTART,
    S_HOME_LINE_SEARCH,
    S_HOME_LINE_SUBS,
    S_HOME_LINE_SIGNIN,

    /* --- hint bars ---------------------------------------------------- */
    S_HINT_CHANNEL_UNSUB,
    S_HINT_CHANNEL_SUB,
    S_HINT_SEARCH_CHANNELS,
    S_HINT_SEARCH_VIDEOS,
    S_HINT_SUBS_CHANNELS_RECENT,
    S_HINT_SUBS_CHANNELS_ALPHA,
    S_HINT_SUBS_VIDEOS,
    S_HINT_HOME,

    /* --- sign in / out ------------------------------------------------ */
    S_SIGNIN_TITLE,
    S_SIGNIN_STARTING,
    S_SIGNIN_IMPOSSIBLE,
    S_GOOGLE_UNREACHABLE,
    S_SIGNIN_WITH_GOOGLE,
    S_SIGNIN_INSTRUCTION,
    S_SIGNED_IN,
    S_SIGNED_IN_NOT_SAVED,
    S_SIGNIN_AGAIN_AFTER_RESTART,
    S_SIGNIN_CANCELLED,
    S_CODE_EXPIRED,
    S_TRY_AGAIN,
    S_YOU_ARE_SIGNED_IN,
    S_SUBS_APPEAR_IN_FEED,
    S_HINT_SIGNOUT,
    S_REALLY_SIGN_OUT,
    S_SIGNOUT_CONSEQUENCE,
    S_HINT_CONFIRM_SIGNOUT,
    S_SIGNING_OUT,
    S_SIGNED_OUT,
    S_REVOKE_FAILED,

    /* --- player ------------------------------------------------------- */
    S_LOADING_VIDEO,
    S_VIDEO_UNPLAYABLE,
    S_NETWORK_ERROR,
    S_HD_UNAVAILABLE,
    S_STARTING_LOWER_QUALITY,
    S_SEEKING,
    S_RESUMING,
    S_HD_PLAYBACK_BROKEN,
    S_CONTINUE_IN,
    S_HD_STREAM_FAILED,
    S_QUALITY_SWITCH_FAILED,
    S_PLAYBACK_ERROR,
    S_VIDEO_ENDED,
    S_SWITCHING_TO,
    S_SWITCHING_TO_HD,
    S_QUALITY_THIS_VIDEO_ONLY,
    S_HINT_PLAYER_PAUSED,
    S_HINT_PLAYER_PLAYING,
    S_HINT_PLAYER_PAUSED_PLAIN,
    S_HINT_PLAYER_PLAYING_PLAIN,
    S_PREPARING_HD,
    S_NO_HD_STREAM,
    S_YOUTUBE_UNREACHABLE,
    S_BUFFERING,
    S_LIVE,

    /* --- the Mehr tab -------------------------------------------------- */
    S_ROW_ACCOUNT,
    S_ROW_QUALITY,
    S_ROW_REGION,
    S_ROW_LANGUAGE,
    S_ROW_SORT,
    S_ROW_LOG,
    S_ROW_CACHE,
    S_ROW_NETWORK,
    S_ROW_VERSION,
    S_SIGNED_IN_VALUE,
    S_NOT_SIGNED_IN_VALUE,
    S_QUALITY_STANDARD,
    S_CONNECTED,
    S_DISCONNECTED,
    S_LANG_AUTOMATIC,
    S_SORT_RECENT,
    S_SORT_ALPHA,
    S_CACHE_CONFIRM_TITLE,
    S_CACHE_CLEAR,
    S_CACHE_KEEP,
    S_HINT_SETTINGS_MODAL,
    S_HINT_SETTINGS_ACCOUNT_IN,
    S_HINT_SETTINGS_ACCOUNT_OUT,
    S_HINT_SETTINGS_CHANGE,
    S_HINT_SETTINGS_OPEN,
    S_HINT_SETTINGS_CLEAR,
    S_HINT_SETTINGS_PLAIN,

    /* --- log viewer ---------------------------------------------------- */
    S_INPUT_TOO_LONG,
    S_INPUT_CONFIRM,

    S_LOG_TAIL_SUFFIX,
    S_LOG_EMPTY,
    S_LOG_NO_PREVIOUS,
    S_HINT_LOG_CURRENT,
    S_HINT_LOG_PREVIOUS,
    /* Error texts that reach the screen through yt_last_error(). */
    S_ERR_TRANSPORT,
    S_ERR_HTTP,
    S_ERR_EMPTY_RESPONSE,
    S_ERR_AUTH_REFRESH,
    S_ERR_NO_STREAM_DATA,
    S_SIGNIN_DENIED,

    VT_STR_COUNT
} VtStringId;

const char *vt_str(VtStringId id);

/* Both halves of one entry, for the parity test. Returns NULL out of range. */
const char *vt_str_de(VtStringId id);
const char *vt_str_en(VtStringId id);

#endif
