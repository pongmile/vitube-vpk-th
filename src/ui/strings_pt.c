/* ViTube — the Brazilian Portuguese catalogue. Keys and grouping: ui/strings.h; lookup and
 * fallback: strings_table.c; parity test: tests/test_strings.c.
 *
 * Real UTF-8 rather than hex escapes: a letter a–f right after an escape
 * would be read as part of it ("Gro\xc3\x9f" "britannien" parses as \x9fb),
 * and the toolchain passes UTF-8 literals through as bytes anyway.
 * Designated initialisers keep every text tied to its key; an entry left out
 * is a NULL the test reports, not a silently shifted table. */
#include "ui/strings.h"

const char *const vt_strings_pt[VT_STR_COUNT] = {
    /* --- app-wide --- */
    [S_CANCEL_HINT] = "○ Cancelar",
    [S_CANCELLING] = "Cancelando ...",
    [S_NO_NETWORK] = "Sem rede",
    [S_CONNECT_WIFI] = "Conecte-se ao Wi-Fi e reinicie",
    [S_NET_INIT_FAILED] = "Falha ao inicializar a rede",

    /* --- tabs and the shell --- */
    [S_TAB_HOME] = "Início",
    [S_TAB_SEARCH] = "Pesquisar",
    [S_TAB_SUBS] = "Inscrições",
    [S_TAB_MORE] = "Mais",
    [S_SEARCH_PROMPT] = "Pesquisar no YouTube",
    [S_SEARCHING] = "Pesquisando ...",
    [S_SEARCH_FAILED] = "Falha na pesquisa",
    [S_CHECK_NETWORK] = "Verifique a conexão",
    [S_LOADING_MORE] = "Carregando mais ...",
    [S_LOADING_HOME] = "Carregando Início ...",
    [S_LOADING_NEW_VIDEOS] = "Carregando novos vídeos ...",
    [S_LOADING_CHANNELS] = "Carregando canais ...",
    [S_LOADING_CHANNEL_VIDEOS] = "Carregando vídeos do canal ...",
    [S_CHANNEL_UNAVAILABLE] = "Canal indisponível",
    [S_NOT_SIGNED_IN] = "Não conectado",
    [S_SIGN_IN_MORE_TAB] = "Faça login na guia Mais",
    [S_NO_NEW_VIDEOS] = "Nenhum vídeo novo encontrado",
    [S_UNKNOWN_ERROR] = "Erro desconhecido",
    [S_SUBSCRIBING] = "Inscrevendo ...",
    [S_UNSUBSCRIBING] = "Cancelando inscrição ...",
    [S_SUBSCRIBE_FAILED] = "Falha ao se inscrever",
    [S_UNSUBSCRIBE_FAILED] = "Falha ao cancelar inscrição",
    [S_SUBSCRIBED] = "Inscrito",
    [S_UNSUBSCRIBED] = "Inscrição cancelada",
    [S_CHANNELS_ALPHA] = "Canais: A–Z",
    [S_CHANNELS_RECENT] = "Canais: vídeo mais recente primeiro",
    [S_NEW_VIDEOS] = "Novos vídeos",
    [S_SUFFIX_CHANNELS] = "  • Canais",
    [S_NO_CHANNELS_FOUND] = "Nenhum canal encontrado",
    [S_NO_SUBSCRIBED_CHANNELS] = "Nenhuma inscrição encontrada",
    [S_SEARCH_EMPTY] = "Pressione △ para pesquisar no YouTube",
    [S_NO_VIDEOS] = "Nenhum vídeo",
    [S_RECENTLY_WATCHED] = "Vistos recentemente",
    [S_NOTHING_WATCHED_YET] = "Nada assistido ainda",
    [S_RECENT_SEARCHES] = "Pesquisas recentes",
    [S_NOTHING_SEARCHED_YET] = "Nada pesquisado ainda",
    [S_HINT_HISTORY] = "✕ Reproduzir   △ Pesquisar   START Voltar",
    [S_HINT_SEARCH_RECENT] = "✕ Pesquisar de novo   △ Nova pesquisa   "
                             "SELECT Mais",
    [S_HINT_SEARCHES_VIEW] = "✕ Pesquisar de novo   △ Nova pesquisa   "
                             "START Voltar",
    [S_HINT_SEARCHES_EMPTY] = "△ Nova pesquisa   START Voltar",

    /* --- home empty state --- */
    [S_NO_VIDEOS_LOADED] = "Nenhum vídeo carregado",
    [S_CHECK_WIFI_RESTART] = "Verifique o Wi-Fi e pressione ✕ para recarregar",
    [S_HOME_LINE_SEARCH] = "△  Iniciar pesquisa",
    [S_HOME_LINE_SUBS] = "R1  ir para Inscrições",
    [S_HOME_LINE_SIGNIN] = "SELECT  Mais: fazer login para recomendações e "
                           "inscrições",

    /* --- hint bars --- */
    [S_HINT_CHANNEL_UNSUB] = "○ Voltar   ✕ Reproduzir   □ Cancelar inscrição",
    [S_HINT_CHANNEL_SUB] = "○ Voltar   ✕ Reproduzir   □ Inscrever-se",
    [S_HINT_SEARCH_CHANNELS] = "□ Vídeos   ✕ Abrir canal   △ Pesquisar   "
                               "START Histórico",
    [S_HINT_SEARCH_VIDEOS] = "△ Pesquisar   ✕ Reproduzir   □ Canais (%d)   "
                             "START Histórico",
    [S_HINT_SUBS_CHANNELS_RECENT] = "□ Novos vídeos   ✕ Abrir canal   △ "
                                    "Pesquisar   START A-Z",
    [S_HINT_SUBS_CHANNELS_ALPHA] = "□ Novos vídeos   ✕ Abrir canal   △ "
                                   "Pesquisar   START Recentes",
    [S_HINT_SUBS_VIDEOS] = "□ Canais   ✕ Reproduzir   △ Pesquisar   SELECT "
                           "Mais",
    [S_HINT_HOME] = "△ Pesquisar   ✕ Reproduzir   START Histórico   SELECT "
                    "Mais",

    /* --- sign in / out --- */
    [S_SIGNIN_TITLE] = "Login",
    [S_SIGNIN_STARTING] = "Iniciando login ...",
    [S_SIGNIN_IMPOSSIBLE] = "Não foi possível fazer login",
    [S_GOOGLE_UNREACHABLE] = "Servidores do Google inacessíveis",
    [S_SIGNIN_WITH_GOOGLE] = "Fazer login com o Google",
    [S_SIGNIN_INSTRUCTION] = "Abra %s no celular e digite o código.",
    [S_SIGNED_IN] = "Conectado!",
    [S_SIGNED_IN_NOT_SAVED] = "Conectado — mas não salvo",
    [S_SIGNIN_AGAIN_AFTER_RESTART] = "Faça login de novo após reiniciar "
                                     "(veja log.txt)",
    [S_SIGNIN_CANCELLED] = "Login cancelado",
    [S_CODE_EXPIRED] = "Código expirado",
    [S_TRY_AGAIN] = "Tente novamente",
    [S_YOU_ARE_SIGNED_IN] = "Você está conectado.",
    [S_SUBS_APPEAR_IN_FEED] = "Suas inscrições aparecem no feed de "
                              "Inscrições (□).",
    [S_HINT_SIGNOUT] = "✕ Sair   ○ Voltar",
    [S_REALLY_SIGN_OUT] = "Deseja mesmo sair?",
    [S_SIGNOUT_CONSEQUENCE] = "Inscrições e recomendações deixam de aparecer.",
    [S_HINT_CONFIRM_SIGNOUT] = "✕ Sim, sair   ○ Cancelar",
    [S_SIGNING_OUT] = "Saindo ...",
    [S_SIGNED_OUT] = "Desconectado",
    [S_REVOKE_FAILED] = "Acesso não revogado no Google",

    /* --- player --- */
    [S_LOADING_VIDEO] = "Carregando vídeo ...",
    [S_VIDEO_UNPLAYABLE] = "Não foi possível reproduzir",
    [S_NETWORK_ERROR] = "Erro de rede",
    [S_HD_UNAVAILABLE] = "HD indisponível",
    [S_STARTING_LOWER_QUALITY] = "Iniciando em qualidade mais baixa",
    [S_SEEKING] = "Pulando ...",
    [S_RESUMING] = "Continuando ...",
    [S_HD_PLAYBACK_BROKEN] = "Falha na reprodução em HD",
    [S_CONTINUE_IN] = "Continuando em %s",
    [S_HD_STREAM_FAILED] = "Não foi possível abrir o stream HD",
    [S_QUALITY_SWITCH_FAILED] = "Falha ao trocar a qualidade",
    [S_PLAYBACK_ERROR] = "Erro de reprodução",
    [S_VIDEO_ENDED] = "O vídeo foi interrompido",
    [S_SWITCHING_TO] = "Mudando para %s ...",
    [S_SWITCHING_TO_HD] = "Mudando para 720p HD ...",
    [S_QUALITY_THIS_VIDEO_ONLY] = "Qualidade · só este vídeo",
    [S_HINT_PLAYER_PAUSED] = "✕ Continuar   ○ Voltar   △ Qualidade",
    [S_HINT_PLAYER_PLAYING] = "✕ Pausar   ○ Voltar   △ Qualidade",
    [S_HINT_PLAYER_PAUSED_PLAIN] = "✕ Continuar   ○ Voltar",
    [S_HINT_PLAYER_PLAYING_PLAIN] = "✕ Pausar   ○ Voltar",
    [S_PREPARING_HD] = "Preparando HD ...",
    [S_NO_HD_STREAM] = "Sem stream HD",
    [S_YOUTUBE_UNREACHABLE] = "YouTube inacessível",
    [S_BUFFERING] = "Carregando ...",
    [S_LIVE] = "LIVE",

    /* --- the Mehr tab --- */
    [S_ROW_ACCOUNT] = "Conta",
    [S_ROW_QUALITY] = "Qualidade",
    [S_ROW_REGION] = "Região",
    [S_ROW_LANGUAGE] = "Idioma",
    [S_ROW_SORT] = "Ordem dos canais",
    [S_ROW_LOG] = "Ver log",
    [S_ROW_CACHE] = "Limpar histórico e cache",
    [S_ROW_NETWORK] = "Rede",
    [S_ROW_VERSION] = "Versão",
    [S_SIGNED_IN_VALUE] = "Conectado",
    [S_NOT_SIGNED_IN_VALUE] = "Não conectado",
    [S_QUALITY_STANDARD] = "Padrão",
    [S_CONNECTED] = "Conectada",
    [S_DISCONNECTED] = "Desconectada",
    [S_LANG_AUTOMATIC] = "Automático",
    [S_SORT_RECENT] = "Vídeo mais recente primeiro",
    [S_SORT_ALPHA] = "A–Z",
    [S_CACHE_CONFIRM_TITLE] = "Descartar histórico, pesquisas e imagens?",
    [S_CACHE_CLEAR] = "Limpar",
    [S_CACHE_KEEP] = "Cancelar",
    [S_HINT_SETTINGS_MODAL] = "✕ Escolher   ○ Cancelar",
    [S_HINT_SETTINGS_ACCOUNT_IN] = "✕ Abrir conta   △ Pesquisar",
    [S_HINT_SETTINGS_ACCOUNT_OUT] = "✕ Fazer login   △ Pesquisar",
    [S_HINT_SETTINGS_CHANGE] = "✕ Alterar   △ Pesquisar",
    [S_HINT_SETTINGS_OPEN] = "✕ Abrir   △ Pesquisar",
    [S_HINT_SETTINGS_CLEAR] = "✕ Limpar   △ Pesquisar",
    [S_HINT_SETTINGS_PLAIN] = "△ Pesquisar",

    /* --- log viewer --- */
    [S_INPUT_TOO_LONG] = "Texto muito longo",
    [S_INPUT_CONFIRM] = "Enter: confirmar",
    [S_LOG_TAIL_SUFFIX] = "  · últimos 32 KB",
    [S_LOG_EMPTY] = "O log está vazio",
    [S_LOG_NO_PREVIOUS] = "Sem log da execução anterior",
    [S_HINT_LOG_CURRENT] = "○ Voltar   □ Log atual   ↑↓ Linha   ←→ Página",
    [S_HINT_LOG_PREVIOUS] = "○ Voltar   □ Execução anterior   ↑↓ Linha   "
                            "←→ Página",
    [S_ERR_TRANSPORT] = "Erro de conexão %d",
    [S_ERR_HTTP] = "Erro HTTP %ld",
    [S_ERR_EMPTY_RESPONSE] = "Resposta vazia (%u bytes)",
    [S_ERR_AUTH_REFRESH] = "Não foi possível renovar o login",
    [S_ERR_NO_STREAM_DATA] = "sem dados de stream",
    [S_SIGNIN_DENIED] = "Login recusado no celular",
};
