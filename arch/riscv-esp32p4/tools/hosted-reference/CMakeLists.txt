cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{HOSTED_REFERENCE_ROOT})
    message(FATAL_ERROR "HOSTED_REFERENCE_ROOT must select the pinned external workspace")
endif()
set(EXTRA_COMPONENT_DIRS
    "$ENV{HOSTED_REFERENCE_ROOT}/esp-hosted-d5184217f710d54c01f932e75b4c55e3f4933fc9"
    "$ENV{HOSTED_REFERENCE_ROOT}/remote-0.2.3/esp_wifi_remote")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jc1060_hosted_reference)
