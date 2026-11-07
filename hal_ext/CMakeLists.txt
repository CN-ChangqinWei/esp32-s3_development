# HAL 扩展组件（目录名 hal_ext 是为了避开 ESP-IDF 内置组件 hal —— IDF 组件名取目录基名，
# 同名会静默覆盖内置组件，导致 IDF 自身源码找不到 hal/sha_types.h）。
#
# 目录约定（与个人公共库一致）：
#   hal_ext/<概念>/inc/<概念>.h            抽象父类
#   hal_ext/<概念>/inc/<概念>_esp32c3.h    平台具象派生类
#   hal_ext/<概念>/src/*.c
#
# 抽象层头文件不包含任何 ESP-IDF / FreeRTOS 头，因此驱动依赖放 PRIV_REQUIRES，
# 使用方（main 等）只依赖 hal_ext 的抽象头即可，不会被拖入平台头文件链。

# 新增模块 .c/.h 时自动重跑 CMake，避免 build.ninja 漏收录新文件。
# ESP-IDF 会用 `cmake -P` 早展开组件 CMakeLists（script 模式禁用 CONFIGURE_DEPENDS），
# 故必须按 CMAKE_BUILD_EARLY_EXPANSION 分支降级为普通 GLOB。
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB_RECURSE src "*.c")
    file(GLOB_RECURSE all_headers "*.h")
else()
    file(GLOB_RECURSE src CONFIGURE_DEPENDS "*.c")
    file(GLOB_RECURSE all_headers CONFIGURE_DEPENDS "*.h")
endif()
set(inc_dirs "")
foreach(header ${all_headers})
    get_filename_component(dir ${header} DIRECTORY)
    list(APPEND inc_dirs ${dir})
endforeach()
list(REMOVE_DUPLICATES inc_dirs)

idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS ${inc_dirs}
    # driver：i2c/src/i2c_esp32c3.c 使用 legacy 头 driver/i2c.h，
    # 该头由 driver 组件提供（esp_driver_i2c 只提供新驱动 i2c_master.h 等）。
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_ledc esp_adc freertos driver
)
