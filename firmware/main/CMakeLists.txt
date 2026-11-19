idf_component_register(SRC_DIRS          "."
                       INCLUDE_DIRS      "."
                       PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils"
                       LDFRAGMENTS "linker.lf"
                       REQUIRES esp_http_server esp_wifi json nvs_flash esp_matter esp_matter_controller esp_matter_console app_reset)

if(CONFIG_SPIFFS_ATTESTATION_TRUST_STORE)
    spiffs_create_partition_image(paa_cert ${CMAKE_SOURCE_DIR}/paa_cert FLASH_IN_PROJECT)
endif()

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
