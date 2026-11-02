cmake_minimum_required(VERSION 3.16)
get_filename_component(EMQTT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emqtt_iram_overflow)
if(NOT IDF_TARGET STREQUAL "esp32" OR NOT CONFIG_FREERTOS_UNICORE OR
   NOT CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY OR
   NOT CONFIG_APP_BUILD_TYPE_PURE_RAM_APP)
    message(FATAL_ERROR "This probe requires single-core ESP32 byte-accessible IRAM and a pure RAM app")
endif()
idf_build_get_property(sdk_python PYTHON)
idf_build_get_property(sdk_path IDF_PATH)
execute_process(COMMAND "${sdk_python}" "${EMQTT_SOURCE_DIR}/tools/sdk.py"
    check --path "${sdk_path}" --quiet RESULT_VARIABLE sdk_result)
if(NOT sdk_result STREQUAL "0")
    message(FATAL_ERROR "Prepare the exact SDK using tools/sdk.py")
endif()
