idf_component_register(
    SRCS
        "src/ap_client.c"
        "src/ap_data.c"
        "src/ap_event.c"
        "src/ap_frame.c"
        "src/ap_md5.c"
        "src/ap_ntp.c"
        "src/ap_platform.c"
        "src/ap_plist.c"
        "src/ap_session.c"
        "src/ap_setup.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES fairplay hap lwip mbedtls esp_timer
)
