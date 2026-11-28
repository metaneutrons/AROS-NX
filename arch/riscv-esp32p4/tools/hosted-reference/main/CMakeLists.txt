option(HOSTED_REFERENCE_LIFECYCLE
    "Build the opt-in credential-free Hosted lifecycle observation app" OFF)

if(HOSTED_REFERENCE_LIFECYCLE)
    set(HOSTED_REFERENCE_SOURCE "lifecycle.c")
else()
    set(HOSTED_REFERENCE_SOURCE "reference.c")
endif()

idf_component_register(SRCS "${HOSTED_REFERENCE_SOURCE}" INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_wifi_remote esp_timer
        esp-hosted-d5184217f710d54c01f932e75b4c55e3f4933fc9)
