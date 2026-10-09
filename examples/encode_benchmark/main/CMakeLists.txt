idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../../decode_benchmark/main"
    REQUIRES micro-flac esp_timer
)
