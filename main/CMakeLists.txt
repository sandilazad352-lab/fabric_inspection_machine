idf_component_register(
    SRCS
        "main.c"
        "app_config.c"
        "gateway_state.c"
        "wifi_manager.c"
        "mqtt_manager.c"
        "ota_manager.c"
        "rs232_reader.c"
        "spool_manager.c"
        "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
)
