cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not defined.")
  endif()
endif()

project(twitch_vita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Twitch")
set(VITA_TITLEID  "TWTC00001")
set(VITA_VERSION  "01.00")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=gnu11")
add_link_options("LINKER:-z,noexecstack")

set(TWITCH_EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(TWITCH_FFMPEG_ROOT "${TWITCH_EXTERNAL}/vita-hw-decoder/build/deps/ffmpeg-vita-hw")
set(TWITCH_CURL_ROOT "${TWITCH_EXTERNAL}/vita-https/build/deps/curl-mbedtls")

foreach(marker
    "${TWITCH_FFMPEG_ROOT}/lib/libavcodec.a"
    "${TWITCH_CURL_ROOT}/lib/libcurl.a")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "Missing prerequisite: ${marker}")
  endif()
endforeach()

set(VITA_HW_DECODER_FFMPEG_ROOT "${TWITCH_FFMPEG_ROOT}" CACHE PATH "" FORCE)
set(VITA_HTTPS_CURL_ROOT "${TWITCH_CURL_ROOT}" CACHE PATH "" FORCE)

add_subdirectory("${TWITCH_EXTERNAL}/vita-hw-decoder" "${CMAKE_BINARY_DIR}/packages/vita-hw-decoder")
add_subdirectory("${TWITCH_EXTERNAL}/vita-https"      "${CMAKE_BINARY_DIR}/packages/vita-https")

add_library(twitch_avformat STATIC IMPORTED)
set_target_properties(twitch_avformat PROPERTIES IMPORTED_LOCATION "${TWITCH_FFMPEG_ROOT}/lib/libavformat.a")
add_library(twitch_avcodec STATIC IMPORTED)
set_target_properties(twitch_avcodec PROPERTIES IMPORTED_LOCATION "${TWITCH_FFMPEG_ROOT}/lib/libavcodec.a")
add_library(twitch_avutil STATIC IMPORTED)
set_target_properties(twitch_avutil PROPERTIES IMPORTED_LOCATION "${TWITCH_FFMPEG_ROOT}/lib/libavutil.a")

add_library(twitch_jansson STATIC
  external/jansson/dump.c external/jansson/error.c external/jansson/hashtable.c
  external/jansson/hashtable_seed.c external/jansson/load.c
  external/jansson/memory.c external/jansson/pack_unpack.c
  external/jansson/strbuffer.c external/jansson/strconv.c
  external/jansson/utf.c external/jansson/value.c external/jansson/version.c)
target_include_directories(twitch_jansson PUBLIC external/jansson)
target_compile_definitions(twitch_jansson PRIVATE HAVE_CONFIG_H)
target_compile_options(twitch_jansson PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pic
  $<$<CONFIG:Release>:-O2> -Wno-format-truncation)

file(GLOB WEBP_SRC
  external/webp/src/dec/*.c
  external/webp/src/dsp/*.c
  external/webp/src/utils/*.c
)
list(FILTER WEBP_SRC EXCLUDE REGEX "(_sse|_msa|_mips)")
add_library(twitch_webp STATIC ${WEBP_SRC})
target_include_directories(twitch_webp PUBLIC external/webp external/webp/src)
target_compile_definitions(twitch_webp PRIVATE WEBP_EXTERN=extern)
target_compile_options(twitch_webp PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pic -fno-pie -fno-common
  $<$<CONFIG:Release>:-O2>)

add_executable(${PROJECT_NAME}
  src/main.c
  src/log.c
  src/store.c
  src/net/http.c
  src/twitch/twitch_api.c
  src/twitch/twitch_auth.c
  src/media/vita_decoder.c
  src/media/remote_stream.c
  src/media/hls.c
  src/ui/ui.c
  src/ui/touch.c
  src/ui/text_input.c
  src/ui/thumbs.c
  src/ui/rows_view.c
  src/ui/list_screen.c
  src/ui/channel_screen.c
  src/ui/player_screen.c
  src/ui/player_quality_switch.c
  src/ui/login_screen.c
  src/ui/qrcodegen.c
  src/ui/log_screen.c
  src/ui/strings.c
  src/ui/strings_table.c
  src/ui/strings_es.c
  src/ui/strings_en.c
  src/ui/strings_pt.c
  src/ui/strings_de.c
  src/ui/strings_it.c
  src/ui/strings_fr.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/external/webp/src
  ${TWITCH_FFMPEG_ROOT}/include
  $ENV{VITASDK}/arm-vita-eabi/include/freetype2
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffunction-sections -fdata-sections
  $<$<CONFIG:Release>:-O3>)
target_link_options(${PROJECT_NAME} PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -flto=auto
  -Wl,--gc-sections
  -Wl,--defsym,__sce_headroom=0x2000)

target_link_libraries(${PROJECT_NAME}
  SceDisplay_stub SceCtrl_stub SceNet_stub SceNetCtl_stub
  SceSysmodule_stub ScePower_stub SceRtc_stub
  vita2d
  ScePgf_stub jpeg freetype bz2 png z
  twitch_webp
  VitaHwDecoder::VitaHwDecoder
  VitaHttps::VitaHttps
  twitch_avformat twitch_avcodec twitch_avutil
  twitch_jansson
  SceVideodec_stub SceCodecEngine_stub SceAudiodec_stub
  SceGxm_stub SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceIme_stub
  SceTouch_stub
  SceAudio_stub SceDisplay_stub m pthread
)

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

vita_create_vpk(Twitch.vpk ${VITA_TITLEID} ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE LICENSE licenses/LICENSE-GPL-3.0.txt
  FILE THIRD_PARTY_NOTICES.md licenses/THIRD_PARTY_NOTICES.md
  FILE ${TWITCH_FFMPEG_ROOT}/share/licenses/FFmpeg-LGPL-2.1.txt licenses/FFmpeg-LGPL-2.1.txt
  FILE ${TWITCH_FFMPEG_ROOT}/share/licenses/wiliwili-GPL-3.0.txt licenses/wiliwili-GPL-3.0.txt
  FILE external/vita-https/licenses/certifi-MPL-2.0.txt licenses/certifi-MPL-2.0.txt
  FILE assets/fonts/FONT-LICENSE.txt licenses/FONT-LICENSE.txt
  FILE assets/fonts/EMOJI-LICENSE.txt licenses/EMOJI-LICENSE.txt
  FILE assets/fonts/font.ttf font.ttf
  FILE assets/fonts/emoji.ttf emoji.ttf
  FILE assets/icons/twitch.png twitch.png
  FILE assets/icons/btn_cross.png btn_cross.png
  FILE assets/icons/btn_circle.png btn_circle.png
  FILE assets/icons/btn_triangle.png btn_triangle.png
  FILE assets/icons/btn_square.png btn_square.png
  FILE assets/icons/btn_l.png btn_l.png
  FILE assets/icons/btn_r.png btn_r.png
  FILE assets/icons/btn_start.png btn_start.png
  FILE assets/icons/btn_select.png btn_select.png
)
add_dependencies(Twitch.vpk-vpk ${PROJECT_NAME}.self-self)
