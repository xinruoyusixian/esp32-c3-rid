idf_component_register(
    SRCS
        app_main.c
        crid_sniffer.c
        crid_parser_common.c
        crid_parser_gb46750.c
        crid_parser_astm.c
        crid_parser_gb42590.c
        crid_tracker.c
        crid_display.c
        crid_json.c
        crid_web.c
        crid_nvs.c
        crid_serial.c
        crid_config.c
        crid_messages.c
        crid_wifi.c
        crid_patrol.c
        crid_shutter.c
        crid_mode.c
        crid_led.c
        crid_ext.c
        crid_relay.c
        crid_clock.c
        crid_oled.c
        crid_gfx.c
        crid_button.c
        crid_screen.c
        crid_cli.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES opendroneid
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        log
        driver
        esp_http_server
        esp_hw_support
        esp_system
        app_update
        esp_timer
        bt
        console
        spi_flash
        cjson
        json
        lwip
        esp_http_client
)

target_compile_options(${COMPONENT_LIB} PRIVATE -finput-charset=utf-8 -fexec-charset=utf-8)

