idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES midicore esp_timer esp_psram spi_flash esp_hw_support
    EMBED_FILES "bank_gm.syn" "song_route1.mid")

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../components/midicore/src")
