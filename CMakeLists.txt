# eccles_media - ultra-light media stack. The codecs in src/ are plain C99 with no ESP-IDF
# dependency (they are unit-tested on the host, see test/); only port/ touches IDF.
# Works both as an ESP-IDF component and as a plain CMake library.
set(EM_SRCS
    src/em_io.c src/em_wav.c src/em_png.c src/em_jpeg.c
    src/em_avi.c src/em_mp4.c src/em_demux.c src/em_mp3.c src/em_player.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${EM_SRCS} port/em_port_esp.c
        INCLUDE_DIRS include port
        PRIV_INCLUDE_DIRS src
        REQUIRES hal driver esp_timer heap freertos log
    )
    set(EM_TARGET ${COMPONENT_LIB})
    target_compile_options(${EM_TARGET} PRIVATE -O2 -Wall -Wextra)
else()
    cmake_minimum_required(VERSION 3.13)
    project(EcclesMedia VERSION 1.0.0 LANGUAGES C)

    add_library(eccles_media ${EM_SRCS})
    add_library(eccles::media ALIAS eccles_media)
    target_include_directories(eccles_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(eccles_media PUBLIC c_std_99)
    set(EM_TARGET eccles_media)
endif()

# MP3 playback: drop minimp3.h (CC0, https://github.com/lieff/minimp3) into src/ to enable it.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/minimp3.h)
    target_compile_definitions(${EM_TARGET} PRIVATE EM_WITH_MINIMP3)
endif()
