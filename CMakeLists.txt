cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not defined.")
  endif()
endif()

project(vitube C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "ViTube")
set(VITA_TITLEID  "VTUB00001")
set(VITA_VERSION  "01.10")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=gnu11")
add_link_options("LINKER:-z,noexecstack")

set(VITUBE_EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(VITUBE_FFMPEG_ROOT "${VITUBE_EXTERNAL}/vita-hw-decoder/build/deps/ffmpeg-vita-hw")
set(VITUBE_CURL_ROOT "${VITUBE_EXTERNAL}/vita-https/build/deps/curl-mbedtls")

foreach(marker
    "${VITUBE_FFMPEG_ROOT}/lib/libavcodec.a"
    "${VITUBE_CURL_ROOT}/lib/libcurl.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/sce_modules/reAvPlayer.suprx")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "Missing prerequisite: ${marker}")
  endif()
endforeach()

set(VITA_HW_DECODER_FFMPEG_ROOT "${VITUBE_FFMPEG_ROOT}" CACHE PATH "" FORCE)
set(VITA_HTTPS_CURL_ROOT "${VITUBE_CURL_ROOT}" CACHE PATH "" FORCE)

add_subdirectory("${VITUBE_EXTERNAL}/vita-hw-decoder" "${CMAKE_BINARY_DIR}/packages/vita-hw-decoder")
add_subdirectory("${VITUBE_EXTERNAL}/vita-https"      "${CMAKE_BINARY_DIR}/packages/vita-https")

add_library(vitube_avformat STATIC IMPORTED)
set_target_properties(vitube_avformat PROPERTIES IMPORTED_LOCATION "${VITUBE_FFMPEG_ROOT}/lib/libavformat.a")
add_library(vitube_avcodec STATIC IMPORTED)
set_target_properties(vitube_avcodec PROPERTIES IMPORTED_LOCATION "${VITUBE_FFMPEG_ROOT}/lib/libavcodec.a")
add_library(vitube_avutil STATIC IMPORTED)
set_target_properties(vitube_avutil PROPERTIES IMPORTED_LOCATION "${VITUBE_FFMPEG_ROOT}/lib/libavutil.a")

# jansson is vendored: the vdpm package is built with -fPIC and its GOT
# relocations break vita-elf-create.
add_library(vitube_jansson STATIC
  external/jansson/dump.c external/jansson/error.c external/jansson/hashtable.c
  external/jansson/hashtable_seed.c external/jansson/load.c
  external/jansson/memory.c external/jansson/pack_unpack.c
  external/jansson/strbuffer.c external/jansson/strconv.c
  external/jansson/utf.c external/jansson/value.c external/jansson/version.c)
target_include_directories(vitube_jansson PUBLIC external/jansson)
target_compile_definitions(vitube_jansson PRIVATE HAVE_CONFIG_H)
target_compile_options(vitube_jansson PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pic
  $<$<CONFIG:Release>:-O2> -Wno-format-truncation)

add_executable(${PROJECT_NAME}
  src/main.c
  src/log.c
  src/store.c
  src/net/http.c
  src/yt/parse.c
  src/yt/innertube.c
  src/yt/oauth.c
  src/yt/hd_helper.c
  src/yt/region.c
  src/media/vita_decoder.c
  src/media/remote_stream.c
  src/media/hls.c
  src/ui/ui.c
  src/ui/touch.c
  src/ui/text_input.c
  src/ui/thumbs.c
  src/ui/rows_view.c
  src/ui/list_screen.c
  src/ui/player_screen.c
  src/ui/player_quality_switch.c
  src/ui/player_hud.c
  src/ui/player_touch.c
  src/ui/settings_screen.c
  src/ui/log_screen.c
  src/ui/strings.c
  src/ui/strings_table.c
  src/ui/login_screen.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${VITUBE_FFMPEG_ROOT}/include
  # FreeType headers: ui.c asks the font itself which code points it has a
  # glyph for, instead of guessing by Unicode range (see ui.c).
  $ENV{VITASDK}/arm-vita-eabi/include/freetype2
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffunction-sections -fdata-sections
  $<$<CONFIG:Release>:-O3>)
target_link_options(${PROJECT_NAME} PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -Wl,--gc-sections
  # vita-elf-create appends ~4 KB of SCE metadata (module info plus the
  # import tables, growing with every imported NID) to the END of the first
  # LOAD segment, into the alignment slack before the data segment. ld's
  # armvita script aligns that segment to 64 KB, so the slack is whatever
  # the code size happens to leave over — anywhere from 0 to 64 KB — and an
  # unrelated edit can break the link with "segment 1 overlaps" (it did
  # twice: 88 bytes missing once, 1352 bytes the next time). The script
  # honours __sce_headroom for exactly this: the data segment is pushed out
  # by at least that much past the end of the text, whatever the code size
  # (a whole 64 KB page in practice, since the script re-aligns afterwards).
  -Wl,--defsym,__sce_headroom=0x2000)

target_link_libraries(${PROJECT_NAME}
  SceDisplay_stub SceCtrl_stub SceNet_stub SceNetCtl_stub
  SceSysmodule_stub ScePower_stub SceRtc_stub
  vita2d
  ScePgf_stub jpeg freetype bz2 png z
  VitaHwDecoder::VitaHwDecoder
  VitaHttps::VitaHttps
  vitube_avformat vitube_avcodec vitube_avutil
  vitube_jansson
  SceAvPlayer_stub SceVideodec_stub SceCodecEngine_stub SceAudiodec_stub
  SceGxm_stub SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceIme_stub
  SceTouch_stub
  SceAudio_stub SceDisplay_stub m pthread
)

# h264_vita needs the public application profile: unsafe homebrew SELF,
# no ASLR, maximum heap mode (memory expansion).
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

vita_create_vpk(ViTube.vpk ${VITA_TITLEID} ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_modules/reAvPlayer.suprx modules/reAvPlayer.suprx
  # Licence texts travel with the binary (GPLv3 s.4/s.6): ViTube's own GPL,
  # the notices naming every linked component, FFmpeg's LGPL and wiliwili's
  # GPL (installed by tools/build-ffmpeg.sh next to the libraries), the font
  # licences, and the CA bundle's MPL. THIRD_PARTY_NOTICES.md names where the
  # corresponding source lives.
  FILE LICENSE licenses/LICENSE-GPL-3.0.txt
  FILE THIRD_PARTY_NOTICES.md licenses/THIRD_PARTY_NOTICES.md
  FILE ${VITUBE_FFMPEG_ROOT}/share/licenses/FFmpeg-LGPL-2.1.txt licenses/FFmpeg-LGPL-2.1.txt
  FILE ${VITUBE_FFMPEG_ROOT}/share/licenses/wiliwili-GPL-3.0.txt licenses/wiliwili-GPL-3.0.txt
  FILE external/vita-https/licenses/certifi-MPL-2.0.txt licenses/certifi-MPL-2.0.txt
  FILE assets/FONT-LICENSE.txt licenses/FONT-LICENSE.txt
  FILE assets/EMOJI-LICENSE.txt licenses/EMOJI-LICENSE.txt
  # DejaVu Sans, loaded from app0:font.ttf. Bundled because the system PGF/PVF
  # fonts cannot render crisply at more than one size (see ui.c); licence in
  # assets/FONT-LICENSE.txt.
  FILE assets/font.ttf font.ttf
  # Noto Emoji (monochrome outlines, not the colour bitmap build: vita2d's
  # text path renders 8-bit coverage and cannot show CBDT colour glyphs).
  # Loaded on demand for code points font.ttf has no glyph for.
  FILE assets/emoji.ttf emoji.ttf
)
add_dependencies(ViTube.vpk-vpk ${PROJECT_NAME}.self-self)
