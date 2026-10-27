set(board_source "board.c")
set(usb_source "memo_usb_mic_stub.c")
if(CONFIG_MEMO_VOCAT16)
    set(usb_source "memo_usb_mic.c" "memo_usb_pcm.c")
endif()
if(CONFIG_MEMO_BOARD_SPOTPEAR_1_28)
    set(board_source "board_spotpear.c")
endif()

idf_component_register(SRCS "app.c" "voice.c" "voice_gate.c" "state.c" "power_state.c" "power_source.c" "power_policy.c" "memo_gauge_profile.c" "memo_store.c" "memo_cardless.cpp" "memo_cardless_buffer.c" "memo_cardless_state.c" "${board_source}" ${usb_source} "ui.c" "memo_icons.c" "cat_anim.c" "memo_font.c" "memo_font20.c" "memo_font22.c" "memo_font24.c" "memo_sync_cache.c" "network_sync_state.c" "network_sync.cpp" "memo_network_config.c" "memo_network_config_nvs.c" "memo_service_pairing.cpp" "memo_service_portal.cpp" "memo_record_ids.c" "memo_record_ids_nvs.c" "memo_sd_sync.cpp" "memo_http_transport.cpp" "memo_cardless_record.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "assets/cat/emotion_sleep_284_126.aaf"
        "assets/cat/emotion_blink_slow_284_126.aaf"
        "assets/cat/emotion_blink_cardless_284_126.aaf"
        "assets/cat/emotion_blink1_284_126.aaf"
        "assets/cat/emotion_happy_284_126.aaf"
        "assets/cat/emotion_sad_284_126.aaf"
        "assets/usb_no_host.pcm"
    REQUIRES esp_adc esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_ledc
             esp_driver_sdmmc esp_driver_usb_serial_jtag fatfs sdmmc esp_timer app_update mbedtls
             esp_http_client tcp_transport lwip esp-wifi-connect esp_netif nvs_flash bootloader_support image_player esp_lcd_gc9a01
             usb_device_uac espressif__tinyusb vfs efuse heap)

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC
    "${COMPONENT_DIR}/usb"
    "${PROJECT_DIR}/components/usb_device_uac/tusb_uac")
if(CONFIG_MEMO_VOCAT16)
    target_sources(${tusb_lib} PRIVATE "${COMPONENT_DIR}/usb/usb_descriptors.c")
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_read_sectors" "-Wl,--wrap=sdmmc_write_sectors")
endif()
