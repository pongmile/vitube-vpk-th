/* ViTube — the German catalogue. Keys and grouping: ui/strings.h; lookup and
 * fallback: strings_table.c; parity test: tests/test_strings.c.
 *
 * Real UTF-8 rather than hex escapes: a letter a–f right after an escape
 * would be read as part of it ("Gro\xc3\x9f" "britannien" parses as \x9fb),
 * and the toolchain passes UTF-8 literals through as bytes anyway.
 * Designated initialisers keep every text tied to its key; an entry left out
 * is a NULL the test reports, not a silently shifted table. */
#include "ui/strings.h"

const char *const vt_strings_de[VT_STR_COUNT] = {
    /* --- app-wide --- */
    [S_CANCEL_HINT] = "○ Abbrechen",
    [S_CANCELLING] = "Abbrechen ...",
    [S_NET_INIT_FAILED] = "Netzwerk-Initialisierung fehlgeschlagen",
    [S_RECONNECTING] = "Verbindung wird wiederhergestellt ...",
    [S_WAITING_FOR_WIFI] = "Warte auf WLAN ...",
    [S_RETRY_IN] = "Nächster Versuch in %d s",
    [S_RETRY_ATTEMPT] = "Versuch %d",

    /* --- tabs and the shell --- */
    [S_TAB_HOME] = "Start",
    [S_TAB_SEARCH] = "Suche",
    [S_TAB_SUBS] = "Abos",
    [S_TAB_MORE] = "Mehr",
    [S_SEARCH_PROMPT] = "YouTube durchsuchen",
    [S_SEARCHING] = "Suche läuft ...",
    [S_SEARCH_FAILED] = "Suche fehlgeschlagen",
    [S_CHECK_NETWORK] = "Netzwerk prüfen",
    [S_LOADING_MORE] = "Lade mehr ...",
    [S_LOADING_HOME] = "Start wird geladen ...",
    [S_LOADING_NEW_VIDEOS] = "Lade neue Videos ...",
    [S_LOADING_CHANNELS] = "Lade Kanäle ...",
    [S_LOADING_CHANNEL_VIDEOS] = "Lade Kanalvideos ...",
    [S_CHANNEL_UNAVAILABLE] = "Kanal nicht verfügbar",
    [S_NOT_SIGNED_IN] = "Nicht angemeldet",
    [S_SIGN_IN_MORE_TAB] = "Anmelden im Tab Mehr",
    [S_NO_NEW_VIDEOS] = "Keine neuen Videos gefunden",
    [S_UNKNOWN_ERROR] = "Unbekannter Fehler",
    [S_SUBSCRIBING] = "Abonniere ...",
    [S_UNSUBSCRIBING] = "Abo wird entfernt ...",
    [S_SUBSCRIBE_FAILED] = "Abonnieren fehlgeschlagen",
    [S_UNSUBSCRIBE_FAILED] = "Abbestellen fehlgeschlagen",
    [S_SUBSCRIBED] = "Abonniert",
    [S_UNSUBSCRIBED] = "Abo entfernt",
    [S_CHANNELS_ALPHA] = "Kanäle: alphabetisch",
    [S_CHANNELS_RECENT] = "Kanäle: neuestes Video zuerst",
    [S_NEW_VIDEOS] = "Neue Videos",
    [S_SUFFIX_CHANNELS] = "  • Kanäle",
    [S_NO_CHANNELS_FOUND] = "Keine Kanäle gefunden",
    [S_NO_SUBSCRIBED_CHANNELS] = "Keine abonnierten Kanäle gefunden",
    [S_SEARCH_EMPTY] = "Drücke △ um YouTube zu durchsuchen",
    [S_NO_VIDEOS] = "Keine Videos",
    [S_RECENTLY_WATCHED] = "Zuletzt gesehen",
    [S_NOTHING_WATCHED_YET] = "Noch nichts angesehen",
    [S_RECENT_SEARCHES] = "Letzte Suchen",
    [S_NOTHING_SEARCHED_YET] = "Noch nichts gesucht",
    [S_HINT_HISTORY] = "✕ Abspielen   △ Suche   START Zurück",
    [S_HINT_SEARCH_RECENT] = "✕ Erneut suchen   △ Neue Suche   SELECT Mehr",
    [S_HINT_SEARCHES_VIEW] = "✕ Erneut suchen   △ Neue Suche   START Zurück",
    [S_HINT_SEARCHES_EMPTY] = "△ Neue Suche   START Zurück",

    /* --- home empty state --- */
    [S_NO_VIDEOS_LOADED] = "Keine Videos geladen",
    [S_CHECK_WIFI_RESTART] = "WLAN prüfen, dann ✕ zum erneuten Laden",
    [S_HOME_LINE_SEARCH] = "△  Suche starten",
    [S_HOME_LINE_SUBS] = "R1  zu Abos wechseln",
    [S_HOME_LINE_SIGNIN] = "SELECT  Mehr: anmelden für Empfehlungen & Abos",

    /* --- hint bars --- */
    [S_HINT_CHANNEL_UNSUB] = "○ Zurück   ✕ Abspielen   □ Abbestellen",
    [S_HINT_CHANNEL_SUB] = "○ Zurück   ✕ Abspielen   □ Abonnieren",
    [S_HINT_SEARCH_CHANNELS] = "□ Videos   ✕ Kanal öffnen   △ Suche   "
                               "START Suchverlauf",
    [S_HINT_SEARCH_VIDEOS] = "△ Suche   ✕ Abspielen   □ Kanäle (%d)   "
                             "START Suchverlauf",
    [S_HINT_SUBS_CHANNELS_RECENT] = "□ Neue Videos   ✕ Kanal öffnen   △ "
                                    "Suche   START A-Z",
    [S_HINT_SUBS_CHANNELS_ALPHA] = "□ Neue Videos   ✕ Kanal öffnen   △ "
                                   "Suche   START Neueste",
    [S_HINT_SUBS_VIDEOS] = "□ Kanäle   ✕ Abspielen   △ Suche   SELECT Mehr",
    [S_HINT_HOME] = "△ Suche   ✕ Abspielen   START Verlauf   SELECT Mehr",

    /* --- sign in / out --- */
    [S_SIGNIN_TITLE] = "Anmeldung",
    [S_SIGNIN_STARTING] = "Starte Anmeldung ...",
    [S_SIGNIN_IMPOSSIBLE] = "Anmeldung nicht möglich",
    [S_GOOGLE_UNREACHABLE] = "Google-Server nicht erreichbar",
    [S_SIGNIN_WITH_GOOGLE] = "Mit Google anmelden",
    [S_SIGNIN_INSTRUCTION] = "Öffne %s auf dem Handy und gib den Code ein.",
    [S_SIGNED_IN] = "Angemeldet!",
    [S_SIGNED_IN_NOT_SAVED] = "Angemeldet — aber nicht gespeichert",
    [S_SIGNIN_AGAIN_AFTER_RESTART] = "Nach Neustart erneut anmelden (siehe "
                                     "log.txt)",
    [S_SIGNIN_CANCELLED] = "Anmeldung abgebrochen",
    [S_CODE_EXPIRED] = "Code abgelaufen",
    [S_TRY_AGAIN] = "Bitte erneut versuchen",
    [S_YOU_ARE_SIGNED_IN] = "Du bist angemeldet.",
    [S_SUBS_APPEAR_IN_FEED] = "Abos erscheinen im Abo-Feed (□).",
    [S_HINT_SIGNOUT] = "✕ Abmelden   ○ Zurück",
    [S_REALLY_SIGN_OUT] = "Wirklich abmelden?",
    [S_SIGNOUT_CONSEQUENCE] = "Abos und Empfehlungen sind danach weg.",
    [S_HINT_CONFIRM_SIGNOUT] = "✕ Ja, abmelden   ○ Abbrechen",
    [S_SIGNING_OUT] = "Melde ab ...",
    [S_SIGNED_OUT] = "Abgemeldet",
    [S_REVOKE_FAILED] = "Zugriff bei Google nicht widerrufen",

    /* --- player --- */
    [S_LOADING_VIDEO] = "Video wird geladen ...",
    [S_VIDEO_UNPLAYABLE] = "Video nicht abspielbar",
    [S_NETWORK_ERROR] = "Netzwerkfehler",
    [S_HD_UNAVAILABLE] = "HD nicht verfügbar",
    [S_STARTING_LOWER_QUALITY] = "Starte in niedrigerer Qualität",
    [S_SEEKING] = "Springe ...",
    [S_RESUMING] = "Setze fort ...",
    [S_HD_PLAYBACK_BROKEN] = "HD-Wiedergabe gestört",
    [S_CONTINUE_IN] = "Weiter in %s",
    [S_HD_STREAM_FAILED] = "HD-Stream konnte nicht geöffnet werden",
    [S_QUALITY_SWITCH_FAILED] = "Qualitätswechsel fehlgeschlagen",
    [S_PLAYBACK_ERROR] = "Wiedergabefehler",
    [S_VIDEO_ENDED] = "Video wurde beendet",
    [S_SWITCHING_TO] = "Wechsle auf %s ...",
    [S_SWITCHING_TO_HD] = "Wechsle auf 720p HD ...",
    [S_QUALITY_THIS_VIDEO_ONLY] = "Qualität · nur dieses Video",
    [S_HINT_PLAYER_PAUSED] = "✕ Weiter   ○ Zurück   △ Qualität",
    [S_HINT_PLAYER_PLAYING] = "✕ Pause   ○ Zurück   △ Qualität",
    [S_HINT_PLAYER_PAUSED_PLAIN] = "✕ Weiter   ○ Zurück",
    [S_HINT_PLAYER_PLAYING_PLAIN] = "✕ Pause   ○ Zurück",
    [S_PREPARING_HD] = "HD wird vorbereitet ...",
    [S_NO_HD_STREAM] = "Kein HD-Stream",
    [S_YOUTUBE_UNREACHABLE] = "YouTube nicht erreichbar",
    [S_BUFFERING] = "Puffern ...",
    [S_LIVE] = "LIVE",

    /* --- the Mehr tab --- */
    [S_ROW_ACCOUNT] = "Konto",
    [S_ROW_QUALITY] = "Videoqualität",
    [S_ROW_REGION] = "Region",
    [S_ROW_LANGUAGE] = "Sprache",
    [S_ROW_SORT] = "Kanalsortierung",
    [S_ROW_LOG] = "Log ansehen",
    [S_ROW_CACHE] = "Verlauf & Cache leeren",
    [S_ROW_NETWORK] = "Netzwerk",
    [S_ROW_VERSION] = "Version",
    [S_SIGNED_IN_VALUE] = "Angemeldet",
    [S_NOT_SIGNED_IN_VALUE] = "Nicht angemeldet",
    [S_QUALITY_STANDARD] = "Standard",
    [S_CONNECTED] = "Verbunden",
    [S_DISCONNECTED] = "Getrennt",
    [S_LANG_AUTOMATIC] = "Automatisch",
    [S_SORT_RECENT] = "Neuestes Video zuerst",
    [S_SORT_ALPHA] = "Alphabetisch",
    [S_CACHE_CONFIRM_TITLE] = "Verlauf, Suchen und Bilder verwerfen?",
    [S_CACHE_CLEAR] = "Leeren",
    [S_CACHE_KEEP] = "Abbrechen",
    [S_HINT_SETTINGS_MODAL] = "✕ Auswählen   ○ Abbrechen",
    [S_HINT_SETTINGS_ACCOUNT_IN] = "✕ Konto öffnen   △ Suche",
    [S_HINT_SETTINGS_ACCOUNT_OUT] = "✕ Anmelden   △ Suche",
    [S_HINT_SETTINGS_CHANGE] = "✕ Ändern   △ Suche",
    [S_HINT_SETTINGS_OPEN] = "✕ Öffnen   △ Suche",
    [S_HINT_SETTINGS_CLEAR] = "✕ Leeren   △ Suche",
    [S_HINT_SETTINGS_PLAIN] = "△ Suche",

    /* --- log viewer --- */
    [S_INPUT_TOO_LONG] = "Eingabe zu lang",
    [S_INPUT_CONFIRM] = "Enter: Bestätigen",
    [S_LOG_TAIL_SUFFIX] = "  · letzte 32 KB",
    [S_LOG_EMPTY] = "Log ist leer",
    [S_LOG_NO_PREVIOUS] = "Kein Log des vorherigen Starts",
    [S_HINT_LOG_CURRENT] = "○ Zurück   □ Aktuelles Log   ↑↓ Zeile   ←→ Seite",
    [S_HINT_LOG_PREVIOUS] = "○ Zurück   □ Vorheriger Start   ↑↓ Zeile   ←→ "
                            "Seite",
    [S_ERR_TRANSPORT] = "Verbindungsfehler %d",
    [S_ERR_HTTP] = "HTTP-Fehler %ld",
    [S_ERR_EMPTY_RESPONSE] = "Leere Antwort (%u Bytes)",
    [S_ERR_AUTH_REFRESH] = "Anmeldung konnte nicht erneuert werden",
    [S_ERR_NO_STREAM_DATA] = "keine Streamdaten",
    [S_SIGNIN_DENIED] = "Anmeldung auf dem Handy abgelehnt",

    /* --- offline playback --- */
    [S_PLAYER_OFFLINE] = "Offline",
    [S_PLAYER_DL_DAMAGED] = "Download beschädigt",
    [S_PLAYER_DL_DAMAGED_HINT] = "Bitte erneut herunterladen",
    /* === BEGIN downloads UI (see strings.h) === */
    /* --- downloads: tab and storage --- */
    [S_TAB_DOWNLOADS] = "Downloads",
    [S_DL_DECIMAL_MARK] = ",",
    [S_DL_STORAGE] = "Frei %s von %s · Downloads %s",
    [S_DL_STORAGE_LOW] = "Speicherkarte voll: nur noch %s frei · Downloads %s",
    [S_DL_FULL_FREE] = "Speicherkarte voll: nur noch %s frei",
    /* --- downloads: empty state --- */
    [S_DL_EMPTY_TITLE] = "Noch keine Downloads",
    [S_DL_EMPTY_LINE1] = "Markiere ein Video in einer Liste und drücke "
                         "SELECT,",
    [S_DL_EMPTY_LINE2] = "dann Herunterladen wählen. So läuft es später ohne "
                         "WLAN.",
    /* --- downloads: status line --- */
    [S_DL_ST_QUEUED] = "In der Warteschlange",
    [S_DL_ST_RESOLVING] = "Wird vorbereitet ...",
    [S_DL_ST_PERCENT] = "%d %%",
    [S_DL_ST_OF] = "%s von %s",
    [S_DL_ST_PAUSED] = "Pausiert",
    [S_DL_ST_WAIT_PLAYBACK] = "Wartet, bis das Video zu Ende ist",
    [S_DL_ST_RETRY_IN] = "Neuer Versuch in %s",
    [S_DL_ST_RETRY_NOW] = "Neuer Versuch ...",
    [S_DL_ST_VERIFYING] = "Datei wird geprüft ...",
    [S_DL_ST_READY] = "Fertig",
    /* --- downloads: why one failed --- */
    [S_DL_ERR_NO_SPACE] = "Braucht %s, nur %s frei",
    [S_DL_ERR_SPACE_FREED] = "Jetzt ist Platz: Erneut versuchen",
    [S_DL_ERR_DISK_FULL] = "Speicherkarte ist voll",
    [S_DL_ERR_NOT_DOWNLOADABLE] = "Dieses Video lässt sich nicht "
                                  "herunterladen",
    [S_DL_ERR_VERIFY] = "Datei ist beschädigt — Erneut versuchen lädt sie neu",
    [S_DL_ERR_NETWORK] = "Netzwerkproblem — erneut versuchen, um fortzufahren",
    [S_DL_ERR_URL] = "Videolink nicht erhalten — später erneut versuchen",
    [S_DL_ERR_IO] = "Datei konnte nicht gespeichert werden — erneut versuchen",
    /* --- downloads: the menu --- */
    [S_DLM_TITLE] = "Download",
    [S_DLM_TITLE_FREE] = "Download · %s frei",
    [S_DLM_DOWNLOAD] = "Herunterladen",
    [S_DLM_OPEN] = "In Downloads öffnen",
    [S_DLM_PLAY] = "Abspielen",
    [S_DLM_PAUSE] = "Pausieren",
    [S_DLM_RESUME] = "Fortsetzen",
    [S_DLM_RETRY] = "Erneut versuchen",
    [S_DLM_RETRY_NOW] = "Jetzt versuchen",
    [S_DLM_PAUSE_ALL] = "Alle pausieren",
    [S_DLM_RESUME_ALL] = "Alle fortsetzen",
    [S_DLM_DELETE] = "Download löschen",
    [S_DLM_CONFIRM_DELETE] = "Diesen Download löschen?",
    [S_DLM_YES_DELETE] = "Löschen",
    [S_DLM_KEEP] = "Behalten",
    /* --- downloads: what the app says afterwards --- */
    [S_DLT_QUEUED] = "Zu den Downloads hinzugefügt",
    [S_DLT_EXISTS] = "Schon in den Downloads",
    [S_DLT_BAD] = "Dieses Video kann nicht geladen werden",
    [S_DLT_DELETED] = "Download gelöscht",
    [S_DLT_LIST_FULL] = "Download-Liste ist voll: erst einige löschen",
    /* --- downloads: hint bars --- */
    [S_HINT_DL_ROW] = "✕ %s   SELECT Menü   △ Suche",
    [S_HINT_DL_ROW_PLAIN] = "SELECT Menü   △ Suche",
    [S_HINT_HOME_DL] = "△ Suche   ✕ Abspielen   START Verlauf   SELECT "
                       "Download",
    [S_HINT_HISTORY_DL] = "✕ Abspielen   △ Suche   START Zurück   SELECT "
                          "Download",
    [S_HINT_SEARCH_VIDEOS_DL] = "△ Suche   ✕ Abspielen   □ Kanäle (%d)   "
                                "START Suchverlauf   SELECT Download",
    [S_HINT_SUBS_VIDEOS_DL] = "□ Kanäle   ✕ Abspielen   △ Suche   SELECT "
                              "Download",
    [S_HINT_CHANNEL_UNSUB_DL] = "○ Zurück   ✕ Abspielen   □ Abbestellen   "
                                "SELECT Download",
    [S_HINT_CHANNEL_SUB_DL] = "○ Zurück   ✕ Abspielen   □ Abonnieren   "
                              "SELECT Download",
    /* === END downloads UI === */

    /* === BEGIN paging (see strings.h) === */
    [S_NO_MORE_VIDEOS] = "Keine weiteren Videos",
    /* === END paging === */
};
