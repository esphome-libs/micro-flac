idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES micro-flac
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../src"
)
