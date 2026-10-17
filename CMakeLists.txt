cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32s3)
set(SDKCONFIG_DEFAULTS "firmware/sdkconfig.freenove;firmware/sdkconfig.defaults")
set(DETECT_MODEL pedestrian_detect)
set(BSP esp32_s3_eye_noglib)
set(PARK_DASHBOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/dashboard")

set(EXTRA_COMPONENT_DIRS
    firmware/main
    firmware/components/who_task
    firmware/components/who_peripherals/esp_video_cxx
    firmware/components/who_peripherals/who_lcd
    firmware/components/who_frame_cap
    firmware/components/who_frame_lcd_disp
    firmware/components/who_detect
    firmware/components/who_app/who_detect_app
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock")
project(monitoramento_parque)
