set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)
# 2.16 用 16/24，不再编进 12px 字库（约省 200KB+）。
# pager/pager_icon.c is the launcher mark.
list(REMOVE_ITEM PROJ_SRCS_C ${PROJ_SRC}/pager/pager_font_12.c)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC} ${PROJ_SRC}/pager
    REQUIRES brookesia_core waveshare__esp32_s3_touch_amoled_2_16 esp_wifi esp_netif esp_event
             nvs_flash driver esp_http_server esp_http_client esp_websocket_client mdns json
             esp_codec_dev lvgl__lvgl
    EMBED_TXTFILES "pager/pager_cert.pem"
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)

set_source_files_properties(
    ${PROJ_SRC}/pager/pager_font_16.c
    ${PROJ_SRC}/pager/pager_font_24.c
    PROPERTIES COMPILE_OPTIONS "-O0"
)

set_source_files_properties(
    ${PROJ_SRC}/pager/pager_net.c
    PROPERTIES COMPILE_OPTIONS "-Wno-stringop-truncation"
)
