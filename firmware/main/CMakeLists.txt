# Defines source files, dependencies, and dashboard resources embedded in the firmware
# Adapted from ESP-WHO examples/object_detect/main/CMakeLists.txt with project sources and embedded web resources

# Compile the application modules together as the main ESP-IDF component
set(srcs app_main.cpp frame_cap_pipeline.cpp counting_core_checks.cpp visitor_counter.cpp
         camera_preview.cpp wifi_preview.cpp park_history.cpp park_cloud.cpp
         pir_sensor.cpp park_power.cpp power_monitor.cpp)

# Include directory setup from the original ESP-WHO example - Original logic preserved
# Expose this directory so the application headers can be included by the component
set(include_dirs ./)

# Public detection dependency from the original ESP-WHO example - Original logic preserved
# The lower-level private dependencies in the registration below are project-specific additions
set(requires who_detect_app)

# Model component name selection from the original ESP-WHO example - Original logic preserved
# Accept either the local model name or the component-manager namespaced name selected by the parent project
set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

# ESP-IDF component registration adapted from ESP-WHO with project-specific sources and private dependencies
idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires}
                       PRIV_REQUIRES esp_psram esp_timer espressif__esp_new_jpeg mbedtls
                                     esp_wifi esp_netif esp_event esp_http_server nvs_flash json esp_driver_uart
                                     fatfs sdmmc esp_driver_sdmmc lwip esp_http_client esp-tls esp_driver_gpio)
# Optional model dependency from the original ESP-WHO example - Original logic preserved
idf_component_optional_requires(PUBLIC ${detect_component})

# Resolve dashboard locations from the parent project rather than duplicating web files inside main
get_filename_component(PARK_PUBLIC "${PARK_DASHBOARD_DIR}/public" ABSOLUTE)
get_filename_component(PARK_LOCAL "${PARK_DASHBOARD_DIR}/local-public" ABSOLUTE)

# Fail during configuration if an embedded asset is missing, before attempting compilation
foreach(asset "${PARK_LOCAL}/index.html" "${PARK_LOCAL}/app.js" "${PARK_PUBLIC}/style.css"
              "${PARK_LOCAL}/monitor.js" "${PARK_LOCAL}/monitor.css"
              "${PARK_PUBLIC}/folhagem-v2.webp" "${PARK_PUBLIC}/source-sans-3-v1.woff2" "${PARK_PUBLIC}/favicon.svg")
    if(NOT EXISTS "${asset}")
        message(FATAL_ERROR "Missing dashboard resource: ${asset}. Set PARK_DASHBOARD_DIR to the project dashboard directory.")
    endif()
endforeach()

# Embed web resources into the firmware. RENAME_TO gives wifi_preview.cpp stable linker symbol names
# TEXT resources include a terminating zero, image/font BINARY resources preserve the original bytes
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/index.html" TEXT RENAME_TO park_index)
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/app.js" TEXT RENAME_TO park_app)
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/monitor.js" TEXT RENAME_TO park_monitor_js)
target_add_binary_data(${COMPONENT_LIB} "${PARK_LOCAL}/monitor.css" TEXT RENAME_TO park_monitor_css)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/style.css" TEXT RENAME_TO park_style)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/folhagem-v2.webp" BINARY RENAME_TO park_foliage)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/source-sans-3-v1.woff2" BINARY RENAME_TO park_font)
target_add_binary_data(${COMPONENT_LIB} "${PARK_PUBLIC}/favicon.svg" TEXT RENAME_TO park_favicon)
