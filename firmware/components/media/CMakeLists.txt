cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "src/frames.c" "src/media_avcc.c" "src/media_pattern.c" "src/media_live.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay
    PRIV_REQUIRES esp_h264 heap
)
