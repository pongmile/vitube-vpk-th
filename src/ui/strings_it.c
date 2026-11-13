/* ViTube — the Italian catalogue. Keys and grouping: ui/strings.h; lookup and
 * fallback: strings_table.c; parity test: tests/test_strings.c.
 *
 * Real UTF-8 rather than hex escapes: a letter a–f right after an escape
 * would be read as part of it ("Gro\xc3\x9f" "britannien" parses as \x9fb),
 * and the toolchain passes UTF-8 literals through as bytes anyway.
 * Designated initialisers keep every text tied to its key; an entry left out
 * is a NULL the test reports, not a silently shifted table. */
#include "ui/strings.h"

const char *const vt_strings_it[VT_STR_COUNT] = {
    /* --- app-wide --- */
    [S_CANCEL_HINT] = "○ Annulla",
    [S_CANCELLING] = "Annullamento ...",
    [S_NET_INIT_FAILED] = "Inizializzazione rete non riuscita",
    [S_RECONNECTING] = "Riconnessione in corso ...",
    [S_WAITING_FOR_WIFI] = "In attesa del Wi-Fi ...",
    [S_RETRY_IN] = "Prossimo tentativo tra %d s",
    [S_RETRY_ATTEMPT] = "Tentativo %d",

    /* --- tabs and the shell --- */
    [S_TAB_HOME] = "Home",
    [S_TAB_SEARCH] = "Cerca",
    [S_TAB_SUBS] = "Iscrizioni",
    [S_TAB_MORE] = "Altro",
    [S_SEARCH_PROMPT] = "Cerca su YouTube",
    [S_SEARCHING] = "Ricerca ...",
    [S_SEARCH_FAILED] = "Ricerca non riuscita",
    [S_CHECK_NETWORK] = "Controlla la rete",
    [S_LOADING_MORE] = "Caricamento ...",
    [S_LOADING_HOME] = "Caricamento Home ...",
    [S_LOADING_NEW_VIDEOS] = "Caricamento nuovi video ...",
    [S_LOADING_CHANNELS] = "Caricamento canali ...",
    [S_LOADING_CHANNEL_VIDEOS] = "Caricamento video del canale ...",
    [S_CHANNEL_UNAVAILABLE] = "Canale non disponibile",
    [S_NOT_SIGNED_IN] = "Non collegato",
    [S_SIGN_IN_MORE_TAB] = "Accedi dalla scheda Altro",
    [S_NO_NEW_VIDEOS] = "Nessun nuovo video trovato",
    [S_UNKNOWN_ERROR] = "Errore sconosciuto",
    [S_SUBSCRIBING] = "Iscrizione ...",
    [S_UNSUBSCRIBING] = "Annullamento iscrizione ...",
    [S_SUBSCRIBE_FAILED] = "Iscrizione non riuscita",
    [S_UNSUBSCRIBE_FAILED] = "Iscrizione non annullata",
    [S_SUBSCRIBED] = "Iscritto",
    [S_UNSUBSCRIBED] = "Iscrizione annullata",
    [S_CHANNELS_ALPHA] = "Canali: A–Z",
    [S_CHANNELS_RECENT] = "Canali: dal video più recente",
    [S_NEW_VIDEOS] = "Nuovi video",
    [S_SUFFIX_CHANNELS] = "  • Canali",
    [S_NO_CHANNELS_FOUND] = "Nessun canale trovato",
    [S_NO_SUBSCRIBED_CHANNELS] = "Nessuna iscrizione trovata",
    [S_SEARCH_EMPTY] = "Premi △ per cercare su YouTube",
    [S_NO_VIDEOS] = "Nessun video",
    [S_RECENTLY_WATCHED] = "Guardati di recente",
    [S_NOTHING_WATCHED_YET] = "Nessun video guardato",
    [S_RECENT_SEARCHES] = "Ricerche recenti",
    [S_NOTHING_SEARCHED_YET] = "Ancora nessuna ricerca",
    [S_HINT_HISTORY] = "✕ Riproduci   △ Cerca   START Indietro",
    [S_HINT_SEARCH_RECENT] = "✕ Cerca di nuovo   △ Nuova ricerca   SELECT "
                             "Altro",
    [S_HINT_SEARCHES_VIEW] = "✕ Cerca di nuovo   △ Nuova ricerca   START "
                             "Indietro",
    [S_HINT_SEARCHES_EMPTY] = "△ Nuova ricerca   START Indietro",

    /* --- home empty state --- */
    [S_NO_VIDEOS_LOADED] = "Nessun video caricato",
    [S_CHECK_WIFI_RESTART] = "Controlla il Wi-Fi, poi ✕ per ricaricare",
    [S_HOME_LINE_SEARCH] = "△  Avvia una ricerca",
    [S_HOME_LINE_SUBS] = "R1  vai alle Iscrizioni",
    [S_HOME_LINE_SIGNIN] = "SELECT  Altro: accedi per consigli e Iscrizioni",

    /* --- hint bars --- */
    [S_HINT_CHANNEL_UNSUB] = "○ Indietro   ✕ Riproduci   □ Annulla iscrizione",
    [S_HINT_CHANNEL_SUB] = "○ Indietro   ✕ Riproduci   □ Iscriviti",
    [S_HINT_SEARCH_CHANNELS] = "□ Video   ✕ Apri canale   △ Cerca   START "
                               "Cronologia ricerche",
    [S_HINT_SEARCH_VIDEOS] = "△ Cerca   ✕ Riproduci   □ Canali (%d)   "
                             "START Cronologia ricerche",
    [S_HINT_SUBS_CHANNELS_RECENT] = "□ Nuovi video   ✕ Apri canale   △ "
                                    "Cerca   START A-Z",
    [S_HINT_SUBS_CHANNELS_ALPHA] = "□ Nuovi video   ✕ Apri canale   △ "
                                   "Cerca   START Più recenti",
    [S_HINT_SUBS_VIDEOS] = "□ Canali   ✕ Riproduci   △ Cerca   SELECT Altro",
    [S_HINT_HOME] = "△ Cerca   ✕ Riproduci   START Cronologia   SELECT Altro",

    /* --- sign in / out --- */
    [S_SIGNIN_TITLE] = "Accesso",
    [S_SIGNIN_STARTING] = "Avvio accesso ...",
    [S_SIGNIN_IMPOSSIBLE] = "Impossibile accedere",
    [S_GOOGLE_UNREACHABLE] = "Server Google non raggiungibili",
    [S_SIGNIN_WITH_GOOGLE] = "Accedi con Google",
    [S_SIGNIN_INSTRUCTION] = "Apri %s sul telefono e inserisci il codice.",
    [S_SIGNED_IN] = "Collegato!",
    [S_SIGNED_IN_NOT_SAVED] = "Collegato — ma non salvato",
    [S_SIGNIN_AGAIN_AFTER_RESTART] = "Dopo il riavvio accedi di nuovo "
                                     "(vedi log.txt)",
    [S_SIGNIN_CANCELLED] = "Accesso annullato",
    [S_CODE_EXPIRED] = "Codice scaduto",
    [S_TRY_AGAIN] = "Riprova",
    [S_YOU_ARE_SIGNED_IN] = "Sei collegato.",
    [S_SUBS_APPEAR_IN_FEED] = "Le tue iscrizioni compaiono nel feed "
                              "Iscrizioni (□).",
    [S_HINT_SIGNOUT] = "✕ Esci   ○ Indietro",
    [S_REALLY_SIGN_OUT] = "Vuoi davvero uscire?",
    [S_SIGNOUT_CONSEQUENCE] = "Iscrizioni e consigli non saranno più "
                              "disponibili.",
    [S_HINT_CONFIRM_SIGNOUT] = "✕ Sì, esci   ○ Annulla",
    [S_SIGNING_OUT] = "Uscita ...",
    [S_SIGNED_OUT] = "Scollegato",
    [S_REVOKE_FAILED] = "Autorizzazione non revocata su Google",

    /* --- player --- */
    [S_LOADING_VIDEO] = "Caricamento video ...",
    [S_VIDEO_UNPLAYABLE] = "Video non riproducibile",
    [S_NETWORK_ERROR] = "Errore di rete",
    [S_HD_UNAVAILABLE] = "HD non disponibile",
    [S_STARTING_LOWER_QUALITY] = "Avvio in qualità inferiore",
    [S_SEEKING] = "Salto ...",
    [S_RESUMING] = "Riprendo ...",
    [S_HD_PLAYBACK_BROKEN] = "Riproduzione HD interrotta",
    [S_CONTINUE_IN] = "Continua in %s",
    [S_HD_STREAM_FAILED] = "Impossibile aprire lo stream HD",
    [S_QUALITY_SWITCH_FAILED] = "Cambio qualità non riuscito",
    [S_PLAYBACK_ERROR] = "Errore riproduzione",
    [S_VIDEO_ENDED] = "Video terminato",
    [S_SWITCHING_TO] = "Passaggio a %s ...",
    [S_SWITCHING_TO_HD] = "Passaggio a 720p HD ...",
    [S_QUALITY_THIS_VIDEO_ONLY] = "Qualità · solo questo video",
    [S_HINT_PLAYER_PAUSED] = "✕ Riprendi   ○ Indietro   △ Qualità",
    [S_HINT_PLAYER_PLAYING] = "✕ Pausa   ○ Indietro   △ Qualità",
    [S_HINT_PLAYER_PAUSED_PLAIN] = "✕ Riprendi   ○ Indietro",
    [S_HINT_PLAYER_PLAYING_PLAIN] = "✕ Pausa   ○ Indietro",
    [S_PREPARING_HD] = "Preparazione HD ...",
    [S_NO_HD_STREAM] = "Nessuno stream HD",
    [S_YOUTUBE_UNREACHABLE] = "YouTube non raggiungibile",
    [S_BUFFERING] = "Buffering ...",
    [S_LIVE] = "LIVE",

    /* --- the Mehr tab --- */
    [S_ROW_ACCOUNT] = "Account",
    [S_ROW_QUALITY] = "Qualità video",
    [S_ROW_REGION] = "Regione",
    [S_ROW_LANGUAGE] = "Lingua",
    [S_ROW_SORT] = "Ordine canali",
    [S_ROW_LOG] = "Visualizza log",
    [S_ROW_CACHE] = "Svuota cronologia e cache",
    [S_ROW_NETWORK] = "Rete",
    [S_ROW_VERSION] = "Versione",
    [S_SIGNED_IN_VALUE] = "Collegato",
    [S_NOT_SIGNED_IN_VALUE] = "Non collegato",
    [S_QUALITY_STANDARD] = "Standard",
    [S_CONNECTED] = "Connesso",
    [S_DISCONNECTED] = "Disconnesso",
    [S_LANG_AUTOMATIC] = "Automatica",
    [S_SORT_RECENT] = "Dal video più recente",
    [S_SORT_ALPHA] = "A–Z",
    [S_CACHE_CONFIRM_TITLE] = "Eliminare cronologia, ricerche e immagini?",
    [S_CACHE_CLEAR] = "Svuota",
    [S_CACHE_KEEP] = "Annulla",
    [S_HINT_SETTINGS_MODAL] = "✕ Scegli   ○ Annulla",
    [S_HINT_SETTINGS_ACCOUNT_IN] = "✕ Apri account   △ Cerca",
    [S_HINT_SETTINGS_ACCOUNT_OUT] = "✕ Accedi   △ Cerca",
    [S_HINT_SETTINGS_CHANGE] = "✕ Cambia   △ Cerca",
    [S_HINT_SETTINGS_OPEN] = "✕ Apri   △ Cerca",
    [S_HINT_SETTINGS_CLEAR] = "✕ Svuota   △ Cerca",
    [S_HINT_SETTINGS_PLAIN] = "△ Cerca",

    /* --- log viewer --- */
    [S_INPUT_TOO_LONG] = "Testo troppo lungo",
    [S_INPUT_CONFIRM] = "Enter: Conferma",
    [S_LOG_TAIL_SUFFIX] = "  · ultimi 32 KB",
    [S_LOG_EMPTY] = "Il log è vuoto",
    [S_LOG_NO_PREVIOUS] = "Nessun log dell'avvio precedente",
    [S_HINT_LOG_CURRENT] = "○ Indietro   □ Log attuale   ↑↓ Riga   ←→ Pagina",
    [S_HINT_LOG_PREVIOUS] = "○ Indietro   □ Avvio precedente   ↑↓ Riga   "
                            "←→ Pagina",
    [S_ERR_TRANSPORT] = "Errore di connessione %d",
    [S_ERR_HTTP] = "Errore HTTP %ld",
    [S_ERR_EMPTY_RESPONSE] = "Risposta vuota (%u byte)",
    [S_ERR_AUTH_REFRESH] = "Impossibile rinnovare l'accesso",
    [S_ERR_NO_STREAM_DATA] = "nessun dato di stream",
    [S_SIGNIN_DENIED] = "Accesso rifiutato sul telefono",

    /* --- offline playback --- */
    [S_PLAYER_OFFLINE] = "Offline",
    [S_PLAYER_DL_DAMAGED] = "Download danneggiato",
    [S_PLAYER_DL_DAMAGED_HINT] = "Scaricalo di nuovo",
    /* === BEGIN downloads UI (see strings.h) === */
    /* --- downloads: tab and storage --- */
    [S_TAB_DOWNLOADS] = "Download",
    [S_DL_DECIMAL_MARK] = ",",
    [S_DL_STORAGE] = "Libero %s su %s · Download %s",
    [S_DL_STORAGE_LOW] = "Scheda di memoria piena: restano solo %s · "
                         "Download %s",
    [S_DL_FULL_FREE] = "Scheda di memoria piena: restano solo %s",
    /* --- downloads: empty state --- */
    [S_DL_EMPTY_TITLE] = "Ancora nessun download",
    [S_DL_EMPTY_LINE1] = "Seleziona un video in un elenco e premi SELECT,",
    [S_DL_EMPTY_LINE2] = "poi scegli Scarica. Si riprodurrà poi senza Wi-Fi.",
    /* --- downloads: status line --- */
    [S_DL_ST_QUEUED] = "In coda",
    [S_DL_ST_RESOLVING] = "Preparazione ...",
    [S_DL_ST_PERCENT] = "%d %%",
    [S_DL_ST_OF] = "%s di %s",
    [S_DL_ST_PAUSED] = "In pausa",
    [S_DL_ST_WAIT_PLAYBACK] = "In attesa che il video finisca",
    [S_DL_ST_RETRY_IN] = "Nuovo tentativo tra %s",
    [S_DL_ST_RETRY_NOW] = "Nuovo tentativo ...",
    [S_DL_ST_VERIFYING] = "Controllo del file ...",
    [S_DL_ST_READY] = "Pronto",
    /* --- downloads: why one failed --- */
    [S_DL_ERR_NO_SPACE] = "Servono %s, liberi solo %s",
    [S_DL_ERR_SPACE_FREED] = "Ora c'è spazio: riprova",
    [S_DL_ERR_DISK_FULL] = "La scheda di memoria è piena",
    [S_DL_ERR_NOT_DOWNLOADABLE] = "Questo video non può essere scaricato",
    [S_DL_ERR_VERIFY] = "File danneggiato — riprova per scaricarlo di nuovo",
    [S_DL_ERR_NETWORK] = "Problema di rete — riprova per continuare",
    [S_DL_ERR_URL] = "Link del video non ottenuto — riprova più tardi",
    [S_DL_ERR_IO] = "Impossibile salvare il file — riprova",
    /* --- downloads: the menu --- */
    [S_DLM_TITLE] = "Download",
    [S_DLM_TITLE_FREE] = "Download · %s liberi",
    [S_DLM_DOWNLOAD] = "Scarica",
    [S_DLM_OPEN] = "Apri in Download",
    [S_DLM_PLAY] = "Riproduci",
    [S_DLM_PAUSE] = "Pausa",
    [S_DLM_RESUME] = "Riprendi",
    [S_DLM_RETRY] = "Riprova",
    [S_DLM_RETRY_NOW] = "Prova ora",
    [S_DLM_PAUSE_ALL] = "Pausa tutto",
    [S_DLM_RESUME_ALL] = "Riprendi tutto",
    [S_DLM_DELETE] = "Elimina download",
    [S_DLM_CONFIRM_DELETE] = "Eliminare questo download?",
    [S_DLM_YES_DELETE] = "Elimina",
    [S_DLM_KEEP] = "Mantieni",
    /* --- downloads: what the app says afterwards --- */
    [S_DLT_QUEUED] = "Aggiunto ai download",
    [S_DLT_EXISTS] = "Già nei download",
    [S_DLT_BAD] = "Impossibile scaricare questo video",
    [S_DLT_DELETED] = "Download eliminato",
    [S_DLT_LIST_FULL] = "Elenco dei download pieno: eliminane qualcuno",
    /* --- downloads: hint bars --- */
    [S_HINT_DL_ROW] = "✕ %s   SELECT Menu   △ Cerca",
    [S_HINT_DL_ROW_PLAIN] = "SELECT Menu   △ Cerca",
    [S_HINT_HOME_DL] = "△ Cerca   ✕ Riproduci   START Cronologia   SELECT "
                       "Scarica",
    [S_HINT_HISTORY_DL] = "✕ Riproduci   △ Cerca   START Indietro   SELECT "
                          "Scarica",
    [S_HINT_SEARCH_VIDEOS_DL] = "△ Cerca   ✕ Riproduci   □ Canali (%d)   "
                                "START Cronologia ricerche   SELECT Scarica",
    [S_HINT_SUBS_VIDEOS_DL] = "□ Canali   ✕ Riproduci   △ Cerca   SELECT "
                              "Scarica",
    [S_HINT_CHANNEL_UNSUB_DL] = "○ Indietro   ✕ Riproduci   □ Annulla "
                                "iscrizione   SELECT Scarica",
    [S_HINT_CHANNEL_SUB_DL] = "○ Indietro   ✕ Riproduci   □ Iscriviti   "
                              "SELECT Scarica",
    /* === END downloads UI === */

    /* === BEGIN paging (see strings.h) === */
    [S_NO_MORE_VIDEOS] = "Nessun altro video",
    /* === END paging === */
};
