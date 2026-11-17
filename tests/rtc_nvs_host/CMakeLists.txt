cmake_minimum_required(VERSION 3.16)
project(passport_rtc_nvs_host C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IDF_PATH "$ENV{IDF_PATH}" CACHE PATH "ESP-IDF source directory")
if(NOT EXISTS "${IDF_PATH}/components/esp_common/include/esp_idf_version.h")
    message(FATAL_ERROR "Set IDF_PATH to ESP-IDF 5.5.6")
endif()
file(READ "${IDF_PATH}/components/esp_common/include/esp_idf_version.h" idf_version_header)
foreach(version_part MAJOR MINOR PATCH)
    if(version_part STREQUAL "PATCH")
        set(expected_version 6)
    else()
        set(expected_version 5)
    endif()
    if(NOT idf_version_header MATCHES "#define ESP_IDF_VERSION_${version_part}[ \t]+${expected_version}([ \r\n]|$)")
        message(FATAL_ERROR "RTC NVS capacity proof requires ESP-IDF 5.5.6")
    endif()
endforeach()
add_library(idf_nvs_engine STATIC
    "${IDF_PATH}/components/nvs_flash/src/nvs_storage.cpp"
    "${IDF_PATH}/components/nvs_flash/src/nvs_pagemanager.cpp"
    "${IDF_PATH}/components/nvs_flash/src/nvs_page.cpp"
    "${IDF_PATH}/components/nvs_flash/src/nvs_item_hash_list.cpp"
    "${IDF_PATH}/components/nvs_flash/src/nvs_types.cpp"
    "${IDF_PATH}/components/esp_rom/linux/esp_rom_crc.c"
)
target_include_directories(idf_nvs_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${IDF_PATH}/components/nvs_flash/src"
    "${IDF_PATH}/components/nvs_flash/include"
    "${IDF_PATH}/components/nvs_flash/private_include"
    "${IDF_PATH}/components/esp_common/include"
    "${IDF_PATH}/components/esp_rom/include"
)
target_compile_definitions(idf_nvs_engine PUBLIC
    NO_DEBUG_STORAGE=1 LINUX_TARGET=1 CONFIG_NVS_ASSERT_ERROR_CHECK=1
)
target_compile_options(idf_nvs_engine PUBLIC -UNDEBUG)
enable_testing()
add_executable(rtc_history_capacity
    test_capacity.cpp
    ../../main/rtc_history_store.c
    ../../main/rtc_history.c
    ../../main/rtc_drift.c
)
target_include_directories(rtc_history_capacity PRIVATE ../../main)
target_link_libraries(rtc_history_capacity PRIVATE idf_nvs_engine)
target_compile_options(rtc_history_capacity PRIVATE -Wall -Wextra -Werror)
add_test(NAME rtc_history_capacity COMMAND rtc_history_capacity)
set_tests_properties(rtc_history_capacity PROPERTIES TIMEOUT 60)
