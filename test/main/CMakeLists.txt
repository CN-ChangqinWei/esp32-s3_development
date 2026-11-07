file(GLOB src "*.c")

idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS "."
    REQUIRES unity hal_ext
)
