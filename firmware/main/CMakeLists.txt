set(srcs app_main.cpp frame_cap_pipeline.cpp counting_core_checks.cpp visitor_counter.cpp
         camera_preview.cpp wifi_preview.cpp park_history.cpp park_cloud.cpp
         pir_sensor.cpp park_power.cpp power_monitor.cpp)

set(include_dirs    ./)

set(requires who_detect_app)

set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires}
                       PRIV_REQUIRES esp_psram esp_timer espressif__esp_new_jpeg mbedtls
                                     esp_wifi esp_netif esp_event esp_http_server nvs_flash json esp_driver_uart
                                     fatfs sdmmc esp_driver_sdmmc lwip esp_http_client esp-tls esp_driver_gpio)
idf_component_optional_requires(PUBLIC ${detect_component})
get_filename_component(PARK_PUBLIC "${PARK_DASHBOARD_DIR}/public" ABSOLUTE)
get_filename_component(PARK_LOCAL "${PARK_DASHBOARD_DIR}/local-public" ABSOLUTE)
foreach(asset "${PARK_LOCAL}/index.html" "${PARK_LOCAL}/app.js" "${PARK_PUBLIC}/style.css"
              "${PARK_LOCAL}/monitor.js" "${PARK_LOCAL}/monitor.css"
              "${PARK_PUBLIC}/folhagem-v2.webp" "${PARK_PUBLIC}/source-sans-3-v1.woff2")
    if(NOT EXISTS "${asset}")
        message(FATAL_ERROR "Missing dashboard resource: ${asset}. Set PARK_DASHBOARD_DIR to the project dashboard directory.")
    endif()
endforeach()
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/index.html" TEXT RENAME_TO park_index)
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/app.js" TEXT RENAME_TO park_app)

target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/monitor.js" TEXT RENAME_TO park_monitor_js)
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/monitor.css" TEXT RENAME_TO park_monitor_css)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/style.css" TEXT RENAME_TO park_style)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/folhagem-v2.webp" BINARY RENAME_TO park_foliage)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/source-sans-3-v1.woff2" BINARY RENAME_TO park_font)
