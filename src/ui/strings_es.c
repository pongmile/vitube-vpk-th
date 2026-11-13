/* ViTube — the Spanish catalogue. Keys and grouping: ui/strings.h; lookup and
 * fallback: strings_table.c; parity test: tests/test_strings.c.
 *
 * Real UTF-8 rather than hex escapes: a letter a–f right after an escape
 * would be read as part of it ("Gro\xc3\x9f" "britannien" parses as \x9fb),
 * and the toolchain passes UTF-8 literals through as bytes anyway.
 * Designated initialisers keep every text tied to its key; an entry left out
 * is a NULL the test reports, not a silently shifted table. */
#include "ui/strings.h"

const char *const vt_strings_es[VT_STR_COUNT] = {
    /* --- app-wide --- */
    [S_CANCEL_HINT] = "○ Cancelar",
    [S_CANCELLING] = "Cancelando ...",
    [S_NET_INIT_FAILED] = "Error al inicializar la red",
    [S_RECONNECTING] = "Reconectando ...",
    [S_WAITING_FOR_WIFI] = "Esperando al Wi-Fi ...",
    [S_RETRY_IN] = "Próximo intento en %d s",
    [S_RETRY_ATTEMPT] = "Intento %d",

    /* --- tabs and the shell --- */
    [S_TAB_HOME] = "Inicio",
    [S_TAB_SEARCH] = "Buscar",
    [S_TAB_SUBS] = "Suscripciones",
    [S_TAB_MORE] = "Más",
    [S_SEARCH_PROMPT] = "Buscar en YouTube",
    [S_SEARCHING] = "Buscando ...",
    [S_SEARCH_FAILED] = "Error en la búsqueda",
    [S_CHECK_NETWORK] = "Comprueba tu conexión",
    [S_LOADING_MORE] = "Cargando más ...",
    [S_LOADING_HOME] = "Cargando Inicio ...",
    [S_LOADING_NEW_VIDEOS] = "Cargando vídeos nuevos ...",
    [S_LOADING_CHANNELS] = "Cargando canales ...",
    [S_LOADING_CHANNEL_VIDEOS] = "Cargando vídeos del canal ...",
    [S_CHANNEL_UNAVAILABLE] = "Canal no disponible",
    [S_NOT_SIGNED_IN] = "No conectado",
    [S_SIGN_IN_MORE_TAB] = "Ve a Más para iniciar sesión",
    [S_NO_NEW_VIDEOS] = "No hay vídeos nuevos",
    [S_UNKNOWN_ERROR] = "Error desconocido",
    [S_SUBSCRIBING] = "Suscribiéndote ...",
    [S_UNSUBSCRIBING] = "Anulando suscripción ...",
    [S_SUBSCRIBE_FAILED] = "Error al suscribirse",
    [S_UNSUBSCRIBE_FAILED] = "Error al anular la suscripción",
    [S_SUBSCRIBED] = "Suscrito",
    [S_UNSUBSCRIBED] = "Suscripción anulada",
    [S_CHANNELS_ALPHA] = "Canales: A–Z",
    [S_CHANNELS_RECENT] = "Canales: vídeo más reciente primero",
    [S_NEW_VIDEOS] = "Vídeos nuevos",
    [S_SUFFIX_CHANNELS] = "  • Canales",
    [S_NO_CHANNELS_FOUND] = "No se encontraron canales",
    [S_NO_SUBSCRIBED_CHANNELS] = "No se han encontrado canales suscritos",
    [S_SEARCH_EMPTY] = "Pulsa △ para buscar en YouTube",
    [S_NO_VIDEOS] = "No hay vídeos",
    [S_RECENTLY_WATCHED] = "Vistos recientemente",
    [S_NOTHING_WATCHED_YET] = "Aún no has visto nada",
    [S_RECENT_SEARCHES] = "Búsquedas recientes",
    [S_NOTHING_SEARCHED_YET] = "Aún no has buscado nada",
    [S_HINT_HISTORY] = "✕ Reproducir   △ Buscar   START Atrás",
    [S_HINT_SEARCH_RECENT] = "✕ Buscar de nuevo   △ Nueva búsqueda   "
                             "SELECT Más",
    [S_HINT_SEARCHES_VIEW] = "✕ Buscar de nuevo   △ Nueva búsqueda   START "
                             "Atrás",
    [S_HINT_SEARCHES_EMPTY] = "△ Nueva búsqueda   START Atrás",

    /* --- home empty state --- */
    [S_NO_VIDEOS_LOADED] = "No se han cargado vídeos",
    [S_CHECK_WIFI_RESTART] = "Comprueba el Wi-Fi y pulsa ✕ para recargar",
    [S_HOME_LINE_SEARCH] = "△  Empezar a buscar",
    [S_HOME_LINE_SUBS] = "R1  Ir a Suscripciones",
    [S_HOME_LINE_SIGNIN] = "SELECT  Más: inicia sesión (recomendaciones, "
                           "suscripciones)",

    /* --- hint bars --- */
    [S_HINT_CHANNEL_UNSUB] = "○ Atrás   ✕ Reproducir   □ Anular suscripción",
    [S_HINT_CHANNEL_SUB] = "○ Atrás   ✕ Reproducir   □ Suscribirse",
    [S_HINT_SEARCH_CHANNELS] = "□ Vídeos   ✕ Abrir canal   △ Buscar   "
                               "START Búsquedas",
    [S_HINT_SEARCH_VIDEOS] = "△ Buscar   ✕ Reproducir   □ Canales (%d)   "
                             "START Búsquedas",
    [S_HINT_SUBS_CHANNELS_RECENT] = "□ Vídeos nuevos   ✕ Abrir canal   △ "
                                    "Buscar   START A-Z",
    [S_HINT_SUBS_CHANNELS_ALPHA] = "□ Vídeos nuevos   ✕ Abrir canal   △ "
                                   "Buscar   START Recientes",
    [S_HINT_SUBS_VIDEOS] = "□ Canales   ✕ Reproducir   △ Buscar   SELECT Más",
    [S_HINT_HOME] = "△ Buscar   ✕ Reproducir   START Historial   SELECT Más",

    /* --- sign in / out --- */
    [S_SIGNIN_TITLE] = "Iniciar sesión",
    [S_SIGNIN_STARTING] = "Iniciando sesión ...",
    [S_SIGNIN_IMPOSSIBLE] = "No se puede iniciar sesión",
    [S_GOOGLE_UNREACHABLE] = "Sin conexión con los servidores de Google",
    [S_SIGNIN_WITH_GOOGLE] = "Inicia sesión con Google",
    [S_SIGNIN_INSTRUCTION] = "Abre %s en el móvil e introduce el código.",
    [S_SIGNED_IN] = "¡Conectado!",
    [S_SIGNED_IN_NOT_SAVED] = "Conectado — pero sin guardar",
    [S_SIGNIN_AGAIN_AFTER_RESTART] = "Tras reiniciar, inicia sesión de "
                                     "nuevo (ver log.txt)",
    [S_SIGNIN_CANCELLED] = "Inicio de sesión cancelado",
    [S_CODE_EXPIRED] = "Código caducado",
    [S_TRY_AGAIN] = "Inténtalo de nuevo",
    [S_YOU_ARE_SIGNED_IN] = "Has iniciado sesión.",
    [S_SUBS_APPEAR_IN_FEED] = "Tus suscripciones aparecen en el feed "
                              "Suscripciones (□).",
    [S_HINT_SIGNOUT] = "✕ Cerrar sesión   ○ Atrás",
    [S_REALLY_SIGN_OUT] = "¿Cerrar sesión?",
    [S_SIGNOUT_CONSEQUENCE] = "Las suscripciones y recomendaciones "
                              "desaparecerán.",
    [S_HINT_CONFIRM_SIGNOUT] = "✕ Sí, cerrar sesión   ○ Cancelar",
    [S_SIGNING_OUT] = "Cerrando sesión ...",
    [S_SIGNED_OUT] = "Desconectado",
    [S_REVOKE_FAILED] = "El acceso en Google no se ha revocado",

    /* --- player --- */
    [S_LOADING_VIDEO] = "Cargando vídeo ...",
    [S_VIDEO_UNPLAYABLE] = "Vídeo no reproducible",
    [S_NETWORK_ERROR] = "Error de red",
    [S_HD_UNAVAILABLE] = "HD no disponible",
    [S_STARTING_LOWER_QUALITY] = "Iniciando en menor calidad",
    [S_SEEKING] = "Saltando ...",
    [S_RESUMING] = "Reanudando ...",
    [S_HD_PLAYBACK_BROKEN] = "Fallo en la reproducción HD",
    [S_CONTINUE_IN] = "Continúa en %s",
    [S_HD_STREAM_FAILED] = "No se ha podido abrir el stream HD",
    [S_QUALITY_SWITCH_FAILED] = "Error al cambiar la calidad",
    [S_PLAYBACK_ERROR] = "Error al reproducir",
    [S_VIDEO_ENDED] = "El vídeo se ha detenido",
    [S_SWITCHING_TO] = "Cambiando a %s ...",
    [S_SWITCHING_TO_HD] = "Cambiando a 720p HD ...",
    [S_QUALITY_THIS_VIDEO_ONLY] = "Calidad · solo este vídeo",
    [S_HINT_PLAYER_PAUSED] = "✕ Reanudar   ○ Atrás   △ Calidad",
    [S_HINT_PLAYER_PLAYING] = "✕ Pausa   ○ Atrás   △ Calidad",
    [S_HINT_PLAYER_PAUSED_PLAIN] = "✕ Reanudar   ○ Atrás",
    [S_HINT_PLAYER_PLAYING_PLAIN] = "✕ Pausa   ○ Atrás",
    [S_PREPARING_HD] = "Preparando HD ...",
    [S_NO_HD_STREAM] = "Sin stream HD",
    [S_YOUTUBE_UNREACHABLE] = "Sin conexión con YouTube",
    [S_BUFFERING] = "Cargando ...",
    [S_LIVE] = "LIVE",

    /* --- the Mehr tab --- */
    [S_ROW_ACCOUNT] = "Cuenta",
    [S_ROW_QUALITY] = "Calidad de vídeo",
    [S_ROW_REGION] = "Región",
    [S_ROW_LANGUAGE] = "Idioma",
    [S_ROW_SORT] = "Orden de canales",
    [S_ROW_LOG] = "Ver el log",
    [S_ROW_CACHE] = "Borrar historial y caché",
    [S_ROW_NETWORK] = "Red",
    [S_ROW_VERSION] = "Versión",
    [S_SIGNED_IN_VALUE] = "Conectado",
    [S_NOT_SIGNED_IN_VALUE] = "No conectado",
    [S_QUALITY_STANDARD] = "Estándar",
    [S_CONNECTED] = "Conectada",
    [S_DISCONNECTED] = "Desconectada",
    [S_LANG_AUTOMATIC] = "Automático",
    [S_SORT_RECENT] = "Vídeo más reciente primero",
    [S_SORT_ALPHA] = "A–Z",
    [S_CACHE_CONFIRM_TITLE] = "¿Borrar historial, búsquedas e imágenes?",
    [S_CACHE_CLEAR] = "Borrar",
    [S_CACHE_KEEP] = "Cancelar",
    [S_HINT_SETTINGS_MODAL] = "✕ Elegir   ○ Cancelar",
    [S_HINT_SETTINGS_ACCOUNT_IN] = "✕ Abrir cuenta   △ Buscar",
    [S_HINT_SETTINGS_ACCOUNT_OUT] = "✕ Iniciar sesión   △ Buscar",
    [S_HINT_SETTINGS_CHANGE] = "✕ Cambiar   △ Buscar",
    [S_HINT_SETTINGS_OPEN] = "✕ Abrir   △ Buscar",
    [S_HINT_SETTINGS_CLEAR] = "✕ Borrar   △ Buscar",
    [S_HINT_SETTINGS_PLAIN] = "△ Buscar",

    /* --- log viewer --- */
    [S_INPUT_TOO_LONG] = "Texto demasiado largo",
    [S_INPUT_CONFIRM] = "Enter: confirmar",
    [S_LOG_TAIL_SUFFIX] = "  · últimos 32 KB",
    [S_LOG_EMPTY] = "El log está vacío",
    [S_LOG_NO_PREVIOUS] = "No hay log del arranque anterior",
    [S_HINT_LOG_CURRENT] = "○ Atrás   □ Log actual   ↑↓ Línea   ←→ Página",
    [S_HINT_LOG_PREVIOUS] = "○ Atrás   □ Arranque anterior   ↑↓ Línea   ←→ "
                            "Página",
    [S_ERR_TRANSPORT] = "Error de conexión %d",
    [S_ERR_HTTP] = "Error HTTP %ld",
    [S_ERR_EMPTY_RESPONSE] = "Respuesta vacía (%u bytes)",
    [S_ERR_AUTH_REFRESH] = "No se ha podido renovar la sesión",
    [S_ERR_NO_STREAM_DATA] = "sin datos de stream",
    [S_SIGNIN_DENIED] = "Inicio de sesión rechazado en el móvil",

    /* --- offline playback --- */
    [S_PLAYER_OFFLINE] = "Sin conexión",
    [S_PLAYER_DL_DAMAGED] = "Descarga dañada",
    [S_PLAYER_DL_DAMAGED_HINT] = "Descárgalo de nuevo",
    /* === BEGIN downloads UI (see strings.h) === */
    /* --- downloads: tab and storage --- */
    [S_TAB_DOWNLOADS] = "Descargas",
    [S_DL_DECIMAL_MARK] = ",",
    [S_DL_STORAGE] = "Libre %s de %s · Descargas %s",
    [S_DL_STORAGE_LOW] = "Tarjeta de memoria llena: solo quedan %s · "
                         "Descargas %s",
    [S_DL_FULL_FREE] = "Tarjeta de memoria llena: solo quedan %s",
    /* --- downloads: empty state --- */
    [S_DL_EMPTY_TITLE] = "Aún no hay descargas",
    [S_DL_EMPTY_LINE1] = "Selecciona un vídeo en una lista y pulsa SELECT,",
    [S_DL_EMPTY_LINE2] = "y elige Descargar. Se reproducirá luego sin Wi-Fi.",
    /* --- downloads: status line --- */
    [S_DL_ST_QUEUED] = "En cola",
    [S_DL_ST_RESOLVING] = "Preparando ...",
    [S_DL_ST_PERCENT] = "%d %%",
    [S_DL_ST_OF] = "%s de %s",
    [S_DL_ST_PAUSED] = "En pausa",
    [S_DL_ST_WAIT_PLAYBACK] = "Esperando a que termine el vídeo",
    [S_DL_ST_RETRY_IN] = "Reintentando en %s",
    [S_DL_ST_RETRY_NOW] = "Reintentando ...",
    [S_DL_ST_VERIFYING] = "Comprobando el archivo ...",
    [S_DL_ST_READY] = "Listo",
    /* --- downloads: why one failed --- */
    [S_DL_ERR_NO_SPACE] = "Necesita %s, solo hay %s libres",
    [S_DL_ERR_SPACE_FREED] = "Ya hay espacio: reintenta",
    [S_DL_ERR_DISK_FULL] = "La tarjeta de memoria está llena",
    [S_DL_ERR_NOT_DOWNLOADABLE] = "Este vídeo no se puede descargar",
    [S_DL_ERR_VERIFY] = "El archivo está dañado — reintenta para "
                        "descargarlo de nuevo",
    [S_DL_ERR_NETWORK] = "Problema de red — reintenta para continuar",
    [S_DL_ERR_URL] = "No se obtuvo el enlace del vídeo — reintenta más tarde",
    [S_DL_ERR_IO] = "No se pudo guardar el archivo — reintenta",
    /* --- downloads: the menu --- */
    [S_DLM_TITLE] = "Descarga",
    [S_DLM_TITLE_FREE] = "Descarga · %s libres",
    [S_DLM_DOWNLOAD] = "Descargar",
    [S_DLM_OPEN] = "Abrir en Descargas",
    [S_DLM_PLAY] = "Reproducir",
    [S_DLM_PAUSE] = "Pausar",
    [S_DLM_RESUME] = "Reanudar",
    [S_DLM_RETRY] = "Reintentar",
    [S_DLM_RETRY_NOW] = "Probar ahora",
    [S_DLM_PAUSE_ALL] = "Pausar todo",
    [S_DLM_RESUME_ALL] = "Reanudar todo",
    [S_DLM_DELETE] = "Eliminar descarga",
    [S_DLM_CONFIRM_DELETE] = "¿Eliminar esta descarga?",
    [S_DLM_YES_DELETE] = "Eliminar",
    [S_DLM_KEEP] = "Conservar",
    /* --- downloads: what the app says afterwards --- */
    [S_DLT_QUEUED] = "Añadido a descargas",
    [S_DLT_EXISTS] = "Ya está en descargas",
    [S_DLT_BAD] = "No se puede descargar este vídeo",
    [S_DLT_DELETED] = "Descarga eliminada",
    [S_DLT_LIST_FULL] = "Lista de descargas llena: elimina alguna antes",
    /* --- downloads: hint bars --- */
    [S_HINT_DL_ROW] = "✕ %s   SELECT Menú   △ Buscar",
    [S_HINT_DL_ROW_PLAIN] = "SELECT Menú   △ Buscar",
    [S_HINT_HOME_DL] = "△ Buscar   ✕ Reproducir   START Historial   SELECT "
                       "Descargar",
    [S_HINT_HISTORY_DL] = "✕ Reproducir   △ Buscar   START Atrás   SELECT "
                          "Descargar",
    [S_HINT_SEARCH_VIDEOS_DL] = "△ Buscar   ✕ Reproducir   □ Canales (%d)   "
                                "START Búsquedas   SELECT Descargar",
    [S_HINT_SUBS_VIDEOS_DL] = "□ Canales   ✕ Reproducir   △ Buscar   SELECT "
                              "Descargar",
    [S_HINT_CHANNEL_UNSUB_DL] = "○ Atrás   ✕ Reproducir   □ Anular "
                                "suscripción   SELECT Descargar",
    [S_HINT_CHANNEL_SUB_DL] = "○ Atrás   ✕ Reproducir   □ Suscribirse   "
                              "SELECT Descargar",
    /* === END downloads UI === */

    /* === BEGIN paging (see strings.h) === */
    [S_NO_MORE_VIDEOS] = "No hay más vídeos",
    /* === END paging === */
};
