/* ViTube — the French catalogue. Keys and grouping: ui/strings.h; lookup and
 * fallback: strings_table.c; parity test: tests/test_strings.c.
 *
 * Real UTF-8 rather than hex escapes: a letter a–f right after an escape
 * would be read as part of it ("Gro\xc3\x9f" "britannien" parses as \x9fb),
 * and the toolchain passes UTF-8 literals through as bytes anyway.
 * Designated initialisers keep every text tied to its key; an entry left out
 * is a NULL the test reports, not a silently shifted table. */
#include "ui/strings.h"

const char *const vt_strings_fr[VT_STR_COUNT] = {
    /* --- app-wide --- */
    [S_CANCEL_HINT] = "○ Annuler",
    [S_CANCELLING] = "Annulation ...",
    [S_NO_NETWORK] = "Aucun réseau",
    [S_CONNECT_WIFI] = "Connectez-vous au Wi-Fi et redémarrez",
    [S_NET_INIT_FAILED] = "Échec de l'initialisation du réseau",

    /* --- tabs and the shell --- */
    [S_TAB_HOME] = "Accueil",
    [S_TAB_SEARCH] = "Rechercher",
    [S_TAB_SUBS] = "Abonnements",
    [S_TAB_MORE] = "Plus",
    [S_SEARCH_PROMPT] = "Rechercher sur YouTube",
    [S_SEARCHING] = "Recherche ...",
    [S_SEARCH_FAILED] = "Échec de la recherche",
    [S_CHECK_NETWORK] = "Vérifiez le réseau",
    [S_LOADING_MORE] = "Chargement ...",
    [S_LOADING_HOME] = "Chargement de l'accueil ...",
    [S_LOADING_NEW_VIDEOS] = "Recherche de nouveautés ...",
    [S_LOADING_CHANNELS] = "Chargement des chaînes ...",
    [S_LOADING_CHANNEL_VIDEOS] = "Vidéos de la chaîne ...",
    [S_CHANNEL_UNAVAILABLE] = "Chaîne indisponible",
    [S_NOT_SIGNED_IN] = "Non connecté",
    [S_SIGN_IN_MORE_TAB] = "Connexion dans l'onglet Plus",
    [S_NO_NEW_VIDEOS] = "Aucune nouvelle vidéo trouvée",
    [S_UNKNOWN_ERROR] = "Erreur inconnue",
    [S_SUBSCRIBING] = "Abonnement ...",
    [S_UNSUBSCRIBING] = "Désabonnement ...",
    [S_SUBSCRIBE_FAILED] = "Échec de l'abonnement",
    [S_UNSUBSCRIBE_FAILED] = "Échec du désabonnement",
    [S_SUBSCRIBED] = "Abonné",
    [S_UNSUBSCRIBED] = "Désabonné",
    [S_CHANNELS_ALPHA] = "Chaînes : A–Z",
    [S_CHANNELS_RECENT] = "Chaînes : dernière vidéo d'abord",
    [S_NEW_VIDEOS] = "Nouveautés",
    [S_SUFFIX_CHANNELS] = "  • Chaînes",
    [S_NO_CHANNELS_FOUND] = "Aucune chaîne trouvée",
    [S_NO_SUBSCRIBED_CHANNELS] = "Aucun abonnement trouvé",
    [S_SEARCH_EMPTY] = "Appuyez sur △ pour rechercher sur YouTube",
    [S_NO_VIDEOS] = "Aucune vidéo",
    [S_RECENTLY_WATCHED] = "Vues récemment",
    [S_NOTHING_WATCHED_YET] = "Aucune vidéo regardée",
    [S_RECENT_SEARCHES] = "Recherches récentes",
    [S_NOTHING_SEARCHED_YET] = "Aucune recherche effectuée",
    [S_HINT_HISTORY] = "✕ Lire   △ Rechercher   START Retour",
    [S_HINT_SEARCH_RECENT] = "✕ Relancer   △ Nouvelle recherche   SELECT Plus",
    [S_HINT_SEARCHES_VIEW] = "✕ Relancer   △ Nouvelle recherche   START Retour",
    [S_HINT_SEARCHES_EMPTY] = "△ Nouvelle recherche   START Retour",

    /* --- home empty state --- */
    [S_NO_VIDEOS_LOADED] = "Aucune vidéo chargée",
    [S_CHECK_WIFI_RESTART] = "Vérifiez le Wi-Fi, puis ✕ pour recharger",
    [S_HOME_LINE_SEARCH] = "△  Rechercher",
    [S_HOME_LINE_SUBS] = "R1  aller aux Abonnements",
    [S_HOME_LINE_SIGNIN] = "SELECT  Plus : connexion pour recommandations "
                           "& abonnements",

    /* --- hint bars --- */
    [S_HINT_CHANNEL_UNSUB] = "○ Retour   ✕ Lire   □ Se désabonner",
    [S_HINT_CHANNEL_SUB] = "○ Retour   ✕ Lire   □ S'abonner",
    [S_HINT_SEARCH_CHANNELS] = "□ Vidéos   ✕ Voir la chaîne   △ "
                               "Rechercher   START Historique",
    [S_HINT_SEARCH_VIDEOS] = "△ Rechercher   ✕ Lire   □ Chaînes (%d)   "
                             "START Historique",
    [S_HINT_SUBS_CHANNELS_RECENT] = "□ Nouveautés   ✕ Voir la chaîne   △ "
                                    "Rechercher   START A-Z",
    [S_HINT_SUBS_CHANNELS_ALPHA] = "□ Nouveautés   ✕ Voir la chaîne   △ "
                                   "Rechercher   START Par date",
    [S_HINT_SUBS_VIDEOS] = "□ Chaînes   ✕ Lire   △ Rechercher   SELECT Plus",
    [S_HINT_HOME] = "△ Rechercher   ✕ Lire   START Historique   SELECT Plus",

    /* --- sign in / out --- */
    [S_SIGNIN_TITLE] = "Connexion",
    [S_SIGNIN_STARTING] = "Connexion en cours ...",
    [S_SIGNIN_IMPOSSIBLE] = "Connexion impossible",
    [S_GOOGLE_UNREACHABLE] = "Serveurs Google inaccessibles",
    [S_SIGNIN_WITH_GOOGLE] = "Se connecter avec Google",
    [S_SIGNIN_INSTRUCTION] = "Ouvrez %s sur votre téléphone et saisissez "
                             "le code.",
    [S_SIGNED_IN] = "Connecté !",
    [S_SIGNED_IN_NOT_SAVED] = "Connecté — mais non enregistré",
    [S_SIGNIN_AGAIN_AFTER_RESTART] = "Reconnectez-vous après un "
                                     "redémarrage (voir log.txt)",
    [S_SIGNIN_CANCELLED] = "Connexion annulée",
    [S_CODE_EXPIRED] = "Code expiré",
    [S_TRY_AGAIN] = "Veuillez réessayer",
    [S_YOU_ARE_SIGNED_IN] = "Vous êtes connecté.",
    [S_SUBS_APPEAR_IN_FEED] = "Vos abonnements apparaissent dans le flux "
                              "Abonnements (□).",
    [S_HINT_SIGNOUT] = "✕ Se déconnecter   ○ Retour",
    [S_REALLY_SIGN_OUT] = "Se déconnecter ?",
    [S_SIGNOUT_CONSEQUENCE] = "Vos abonnements et recommandations ne "
                              "s'afficheront plus.",
    [S_HINT_CONFIRM_SIGNOUT] = "✕ Oui, se déconnecter   ○ Annuler",
    [S_SIGNING_OUT] = "Déconnexion ...",
    [S_SIGNED_OUT] = "Déconnecté",
    [S_REVOKE_FAILED] = "L'accès Google n'a pas été révoqué",

    /* --- player --- */
    [S_LOADING_VIDEO] = "Chargement de la vidéo ...",
    [S_VIDEO_UNPLAYABLE] = "Impossible de lire la vidéo",
    [S_NETWORK_ERROR] = "Erreur réseau",
    [S_HD_UNAVAILABLE] = "HD indisponible",
    [S_STARTING_LOWER_QUALITY] = "Démarrage en qualité inférieure",
    [S_SEEKING] = "Saut ...",
    [S_RESUMING] = "Reprise ...",
    [S_HD_PLAYBACK_BROKEN] = "Lecture HD interrompue",
    [S_CONTINUE_IN] = "Reprise en %s",
    [S_HD_STREAM_FAILED] = "Impossible d'ouvrir le flux HD",
    [S_QUALITY_SWITCH_FAILED] = "Échec du changement de qualité",
    [S_PLAYBACK_ERROR] = "Erreur de lecture",
    [S_VIDEO_ENDED] = "La vidéo a été arrêtée",
    [S_SWITCHING_TO] = "Passage en %s ...",
    [S_SWITCHING_TO_HD] = "Passage en 720p HD ...",
    [S_QUALITY_THIS_VIDEO_ONLY] = "Qualité · cette vidéo uniquement",
    [S_HINT_PLAYER_PAUSED] = "✕ Reprendre   ○ Retour   △ Qualité",
    [S_HINT_PLAYER_PLAYING] = "✕ Pause   ○ Retour   △ Qualité",
    [S_HINT_PLAYER_PAUSED_PLAIN] = "✕ Reprendre   ○ Retour",
    [S_HINT_PLAYER_PLAYING_PLAIN] = "✕ Pause   ○ Retour",
    [S_PREPARING_HD] = "Préparation de la HD ...",
    [S_NO_HD_STREAM] = "Aucun flux HD",
    [S_YOUTUBE_UNREACHABLE] = "YouTube inaccessible",
    [S_BUFFERING] = "Chargement ...",
    [S_LIVE] = "LIVE",

    /* --- the Mehr tab --- */
    [S_ROW_ACCOUNT] = "Compte",
    [S_ROW_QUALITY] = "Qualité vidéo",
    [S_ROW_REGION] = "Région",
    [S_ROW_LANGUAGE] = "Langue",
    [S_ROW_SORT] = "Tri des chaînes",
    [S_ROW_LOG] = "Voir le journal",
    [S_ROW_CACHE] = "Vider historique & cache",
    [S_ROW_NETWORK] = "Réseau",
    [S_ROW_VERSION] = "Version",
    [S_SIGNED_IN_VALUE] = "Connecté",
    [S_NOT_SIGNED_IN_VALUE] = "Non connecté",
    [S_QUALITY_STANDARD] = "Standard",
    [S_CONNECTED] = "Connecté",
    [S_DISCONNECTED] = "Déconnecté",
    [S_LANG_AUTOMATIC] = "Automatique",
    [S_SORT_RECENT] = "Dernière vidéo d'abord",
    [S_SORT_ALPHA] = "A–Z",
    [S_CACHE_CONFIRM_TITLE] = "Supprimer historique, recherches et images ?",
    [S_CACHE_CLEAR] = "Vider",
    [S_CACHE_KEEP] = "Annuler",
    [S_HINT_SETTINGS_MODAL] = "✕ Choisir   ○ Annuler",
    [S_HINT_SETTINGS_ACCOUNT_IN] = "✕ Ouvrir le compte   △ Rechercher",
    [S_HINT_SETTINGS_ACCOUNT_OUT] = "✕ Connexion   △ Rechercher",
    [S_HINT_SETTINGS_CHANGE] = "✕ Modifier   △ Rechercher",
    [S_HINT_SETTINGS_OPEN] = "✕ Ouvrir   △ Rechercher",
    [S_HINT_SETTINGS_CLEAR] = "✕ Vider   △ Rechercher",
    [S_HINT_SETTINGS_PLAIN] = "△ Rechercher",

    /* --- log viewer --- */
    [S_INPUT_TOO_LONG] = "Saisie trop longue",
    [S_INPUT_CONFIRM] = "Enter : Valider",
    [S_LOG_TAIL_SUFFIX] = "  · 32 derniers KB",
    [S_LOG_EMPTY] = "Journal vide",
    [S_LOG_NO_PREVIOUS] = "Aucun journal du démarrage précédent",
    [S_HINT_LOG_CURRENT] = "○ Retour   □ Journal actuel   ↑↓ Ligne   ←→ Page",
    [S_HINT_LOG_PREVIOUS] = "○ Retour   □ Démarrage précédent   ↑↓ Ligne   "
                            "←→ Page",
    [S_ERR_TRANSPORT] = "Erreur de connexion %d",
    [S_ERR_HTTP] = "Erreur HTTP %ld",
    [S_ERR_EMPTY_RESPONSE] = "Réponse vide (%u octets)",
    [S_ERR_AUTH_REFRESH] = "Impossible de renouveler la connexion",
    [S_ERR_NO_STREAM_DATA] = "aucune donnée de flux",
    [S_SIGNIN_DENIED] = "Connexion refusée sur le téléphone",
};
