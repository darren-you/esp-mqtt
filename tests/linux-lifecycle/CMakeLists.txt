cmake_minimum_required(VERSION 3.22)
set(COMPONENTS main mqtt)
if(NOT ESP_MQTT_COMPONENT_DIR)
    message(FATAL_ERROR "Pass -D ESP_MQTT_COMPONENT_DIR=<isolated-output>/mqtt")
endif()
set(EXTRA_COMPONENT_DIRS ${ESP_MQTT_COMPONENT_DIR})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_mqtt_linux_lifecycle_probe)
idf_component_get_property(mqtt_lib mqtt COMPONENT_LIB)
target_compile_definitions(${mqtt_lib} PRIVATE
    esp_transport_tcp_init=emqtt_test_tcp_init
    esp_transport_ssl_init=emqtt_test_ssl_init
    esp_transport_list_add=emqtt_test_list_add
    esp_transport_destroy=emqtt_test_transport_destroy)
