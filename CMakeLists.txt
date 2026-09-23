set(srcs mqtt_client.c lib/mqtt_msg.c lib/platform_esp32_idf.c
         runtime/emqtt.c runtime/emqtt_contract.c)

if(CONFIG_MQTT_PROTOCOL_5)
    list(APPEND srcs lib/mqtt5_msg.c mqtt5_client.c)
endif()

list(TRANSFORM srcs PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/runtime/include
                    PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/lib/include
                    REQUIRES esp_event tcp_transport
                    PRIV_REQUIRES esp_timer http_parser esp_hw_support heap
                    KCONFIG ${CMAKE_CURRENT_LIST_DIR}/Kconfig
                    )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(sdk_python PYTHON)
    idf_build_get_property(sdk_path IDF_PATH)
    idf_component_get_property(lwip_component lwip COMPONENT_DIR)
    get_filename_component(actual_lwip_component "${lwip_component}" REALPATH)
    get_filename_component(expected_lwip_component "${sdk_path}/components/lwip" REALPATH)
    if(NOT actual_lwip_component STREQUAL expected_lwip_component)
        message(FATAL_ERROR "ESP MQTT requires the locked SDK lwIP component")
    endif()
    execute_process(COMMAND "${sdk_python}" "${CMAKE_CURRENT_LIST_DIR}/tools/sdk.py"
        check --path "${sdk_path}" --quiet
        RESULT_VARIABLE sdk_result ERROR_VARIABLE sdk_error)
    if(NOT sdk_result STREQUAL "0")
        message(FATAL_ERROR "${sdk_error}\nPrepare the exact SDK with tools/sdk.py prepare")
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/mqtt_utils)
target_link_libraries(${COMPONENT_LIB} PUBLIC idf::mqtt::utils)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/mqtt_outbox)
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::mqtt::outbox)
