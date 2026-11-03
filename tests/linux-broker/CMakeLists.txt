cmake_minimum_required(VERSION 3.22)
set(COMPONENTS main mqtt)
if(NOT ESP_MQTT_COMPONENT_DIR)
    message(FATAL_ERROR "Pass -D ESP_MQTT_COMPONENT_DIR=<isolated-output>/mqtt")
endif()
set(EXTRA_COMPONENT_DIRS ${ESP_MQTT_COMPONENT_DIR})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_mqtt_linux_broker_probe)
if(EMQTT_ENQUEUE_LATENCY_TEST)
    # Redirect only the MQTT caller; the test wrapper calls the real transport.
    idf_component_get_property(mqtt_lib mqtt COMPONENT_LIB)
    target_compile_definitions(${mqtt_lib} PRIVATE
        esp_transport_poll_read=emqtt_test_transport_poll_read)
endif()

if(EMQTT_RECEIVE_BACKPRESSURE_TEST)
    # Observe real transport reads only in the test MQTT caller.
    idf_component_get_property(mqtt_lib mqtt COMPONENT_LIB)
    target_compile_definitions(${mqtt_lib} PRIVATE
        esp_transport_read=emqtt_test_transport_read)
endif()
