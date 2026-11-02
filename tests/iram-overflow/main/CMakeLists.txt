idf_component_register(
    SRCS "overflow_probe.c" "${EMQTT_SOURCE_DIR}/runtime/emqtt.c"
         "${EMQTT_SOURCE_DIR}/runtime/emqtt_contract.c"
    INCLUDE_DIRS "${EMQTT_SOURCE_DIR}/runtime/include" "${EMQTT_SOURCE_DIR}/include"
    REQUIRES esp_timer esp_event tcp_transport heap)
# The injected client does not open a transport. Keep the runtime's normal
# contract gates, permitting only this test's synthetic plaintext config.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_MQTT_REPORT_DELETED_MESSAGES=1 CONFIG_MBEDTLS_HAVE_TIME_DATE=1
    CONFIG_EMQTT_PLAINTEXT_LAB=1)
