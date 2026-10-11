set(src_dirs        ./)

set(include_dirs    ./)

set(requires who_detect_app)

set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires}
                       PRIV_REQUIRES esp_psram esp_timer espressif__esp_new_jpeg mbedtls
                                     esp_wifi esp_netif esp_event esp_http_server nvs_flash json esp_driver_uart
                                     fatfs sdmmc esp_driver_sdmmc lwip)
idf_component_optional_requires(PUBLIC ${detect_component})
get_filename_component(PARK_PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../../../dashboard/public" ABSOLUTE)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/index.html" TEXT RENAME_TO park_index)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/app.js" TEXT RENAME_TO park_app)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/style.css" TEXT RENAME_TO park_style)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/folhagem-v2.webp" BINARY RENAME_TO park_foliage)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/source-sans-3-v1.woff2" BINARY RENAME_TO park_font)
