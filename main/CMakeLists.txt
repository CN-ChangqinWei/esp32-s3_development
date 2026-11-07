# 新增/删除 src/*.c 时自动重跑 CMake，避免 build.ninja 只在 configure 期收录文件、
# 新增源文件漏编后在链接期报 undefined reference。
# 注意：ESP-IDF 会用 `cmake -P` 对组件 CMakeLists 做 early expansion 以抽取依赖，
# 而 script 模式禁止 file(GLOB ... CONFIGURE_DEPENDS)，故必须按该变量降级。
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB_RECURSE src "src/*.c")
else()
    file(GLOB_RECURSE src CONFIGURE_DEPENDS "src/*.c")
endif()
set (inc "inc/")

idf_component_register(SRCS ${src}
                    INCLUDE_DIRS ${inc}
                    REQUIRES tools services hal_ext esp_driver_gpio esp_driver_uart
                    )
