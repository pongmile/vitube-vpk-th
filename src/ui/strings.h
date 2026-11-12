/* ViTube — user-facing text in German, English, Spanish, Portuguese, Italian
 * and French.
 *
 * Every string the app puts on screen goes through vt_str(). Log lines do
 * NOT: they are developer-facing and stay as they are. Neither do the unit
 * words in parse.c's age_minutes() — those read YouTube's own output and have
 * to follow the region's hl, not the interface language.
 *
 * The catalogue is one table per language (strings_de.c ... strings_fr.c),
 * each indexed by the enum below through designated initialisers, so a
 * translation cannot drift out of alignment with its key the way positional
 * arrays can. German is the original. tests/test_strings.c checks that every
 * language has every entry and that printf conversions, button glyphs and key
 * names match the German. */
#ifndef VITUBE_UI_STRINGS_H
#define VITUBE_UI_STRINGS_H

typedef enum {
    VT_LANG_AUTO = 0, /* follow the region's hl */
    VT_LANG_DE,
    VT_LANG_EN,
    /* Append only: lang.txt stores the choice as its index, one digit. */
    VT_LANG_ES,
    VT_LANG_PT, /* Brazilian Portuguese — the variant YouTube's "pt" is */
    VT_LANG_IT,
    VT_LANG_FR,
    VT_LANG_CHOICE_COUNT
} VtLangChoice;

/* Reads the stored choice. Call once at startup, after yt_region_load():
 * VT_LANG_AUTO resolves against the region. */
void vt_lang_load(void);
VtLangChoice vt_lang_choice(void);
void vt_lang_set_choice(VtLangChoice choice);
const char *vt_lang_label(VtLangChoice choice);
/* What the app is actually speaking right now, as that language's own name
 * ("Deutsch", "English", "Español", ...). Shown beside the Automatic entry
 * so that choice is not a promise the user has to go and verify somewhere
 * else. */
const char *vt_lang_effective_label(void);

typedef enum {
    /* --- app-wide ---------------------------------------------------- */
    S_CANCEL_HINT,
    S_CANCELLING,
    S_NET_INIT_FAILED,
    /* The reconnect screen of src/ui/net_task.c: a load that failed because
     * the network was gone (typically just after a wake from suspend) and
     * is being retried rather than reported. */
    S_RECONNECTING,
    S_WAITING_FOR_WIFI,
    S_RETRY_IN,
    S_RETRY_ATTEMPT,

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
    /* For videos whose Triangle menu stays closed (Shorts, live): the legend
     * must not advertise a key that does nothing. */
    S_HINT_PLAYER_PAUSED_PLAIN,
    S_HINT_PLAYER_PLAYING_PLAIN,
    S_PREPARING_HD,
    S_NO_HD_STREAM,
    S_YOUTUBE_UNREACHABLE,
    S_BUFFERING,
    /* Untranslated in every language on purpose: YouTube's own badge reads
     * LIVE everywhere, and the player HUD should say the same word the list
     * did. */
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

    /* --- offline playback (player_screen.c) ---------------------------- */
    /* The tag in the player's top-right badge while a downloaded file plays,
     * in the place of the decoder name: the viewer should be able to tell
     * that nothing is being streamed. Kept short, the badge is a single line
     * that shrinks to fit. */
    S_PLAYER_OFFLINE,
    /* Toast when a download will not play after all (it would not open, or the
     * decoder gave up in its first seconds): the title, and what to do about
     * it. The entry is back on the Downloads list as failed. */
    S_PLAYER_DL_DAMAGED,
    S_PLAYER_DL_DAMAGED_HINT,
    /* --- end of offline playback ---------------------------------------- */
    /* === BEGIN downloads UI (src/ui/downloads_screen.c, list_screen.c) ===
     * One block, appended last, so that the ids of other work end up above or
     * below it as a whole. Hint bars that name SELECT: on a row of a video
     * list that button opens the download menu, which is why each of those
     * lists has a *_DL twin of its hint (the plain one stays for the empty
     * list, where SELECT still goes to the Mehr tab). */

    /* The tab, and the decimal mark vt_fmt_* prints sizes with ("." or ","). */
    S_TAB_DOWNLOADS,
    S_DL_DECIMAL_MARK,

    /* Storage line above the list. Free, total, used by downloads / free
     * (red: the card is below the reserve), used by downloads. */
    S_DL_STORAGE,
    S_DL_STORAGE_LOW,
    /* Also the toast after refusing a download, and the menu's title. */
    S_DL_FULL_FREE,

    /* Nothing downloaded yet: a title and two lines saying how to start. */
    S_DL_EMPTY_TITLE,
    S_DL_EMPTY_LINE1,
    S_DL_EMPTY_LINE2,

    /* The status line of a row. Pieces joined with " · " by the screen:
     * "42 %" + "15.3 of 36.4 MB" + "1.2 MB/s". */
    S_DL_ST_QUEUED,
    S_DL_ST_RESOLVING,
    S_DL_ST_PERCENT,
    S_DL_ST_OF,
    S_DL_ST_PAUSED,
    S_DL_ST_WAIT_PLAYBACK,
    S_DL_ST_RETRY_IN,
    S_DL_ST_RETRY_NOW,
    S_DL_ST_VERIFYING,
    S_DL_ST_READY,

    /* Why a download failed; one per DlError. NO_SPACE: needed, free. */
    S_DL_ERR_NO_SPACE,
    S_DL_ERR_SPACE_FREED,
    S_DL_ERR_DISK_FULL,
    S_DL_ERR_NOT_DOWNLOADABLE,
    S_DL_ERR_VERIFY,
    S_DL_ERR_NETWORK,
    S_DL_ERR_URL,
    S_DL_ERR_IO,

    /* The menu. The action words double as the "✕ %s" of the Downloads
     * hint, so each must read as a verb on its own. */
    S_DLM_TITLE,
    S_DLM_TITLE_FREE,
    S_DLM_DOWNLOAD,
    S_DLM_OPEN,
    S_DLM_PLAY,
    S_DLM_PAUSE,
    S_DLM_RESUME,
    S_DLM_RETRY,
    S_DLM_RETRY_NOW,
    S_DLM_PAUSE_ALL,
    S_DLM_RESUME_ALL,
    S_DLM_DELETE,
    S_DLM_CONFIRM_DELETE,
    S_DLM_YES_DELETE,
    S_DLM_KEEP,

    /* What the app says after an action. */
    S_DLT_QUEUED,
    S_DLT_EXISTS,
    S_DLT_BAD,
    S_DLT_DELETED,

    /* Hint bars. DL_ROW: ✕ gets the action the focused row's state offers. */
    S_HINT_DL_ROW,
    S_HINT_DL_ROW_PLAIN,
    S_HINT_HOME_DL,
    S_HINT_HISTORY_DL,
    S_HINT_SEARCH_VIDEOS_DL,
    S_HINT_SUBS_VIDEOS_DL,
    S_HINT_CHANNEL_UNSUB_DL,
    S_HINT_CHANNEL_SUB_DL,
    /* === END downloads UI === */

    VT_STR_COUNT
} VtStringId;

const char *vt_str(VtStringId id);

/* One language's text for an entry, with neither the AUTO resolution nor the
 * fallback: NULL when `lang` is AUTO or out of range, or when that language
 * has no text for the entry. This is what strings.c and the parity test read. */
const char *vt_str_in(VtLangChoice lang, VtStringId id);
/* "de", "en", "es", ... for log lines and the test's report; "auto" for AUTO. */
const char *vt_lang_code(VtLangChoice lang);

#endif
