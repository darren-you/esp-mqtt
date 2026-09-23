idf_component_register(SRCS "app_main.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES mqtt esp_wifi esp_event esp_netif esp_timer heap esp_driver_usb_serial_jtag)
if(NOT EMQTT_SAMPLE_INPUTS)
    set(EMQTT_SAMPLE_INPUTS "${CMAKE_CURRENT_LIST_DIR}/../inputs.example.h")
endif()
if(NOT EXISTS "${EMQTT_SAMPLE_INPUTS}" OR IS_DIRECTORY "${EMQTT_SAMPLE_INPUTS}")
    message(FATAL_ERROR "EMQTT_SAMPLE_INPUTS must name an existing header")
endif()
configure_file("${EMQTT_SAMPLE_INPUTS}" "${CMAKE_CURRENT_BINARY_DIR}/sample_inputs.h" COPYONLY)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
