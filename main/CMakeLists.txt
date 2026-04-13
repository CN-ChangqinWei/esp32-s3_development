file(GLOB_RECURSE src "src/*.c")
set (inc "inc/")

idf_component_register(SRCS ${src}
                    INCLUDE_DIRS ${inc}
                    REQUIRES tools services
                    )
