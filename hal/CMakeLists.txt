# HAL 组件：外设抽象层（纯 C，无平台依赖）+ ESP32-C3 具象
#
# 目录约定（与个人公共库一致）：
#   hal/<概念>/inc/<概念>.h            抽象父类
#   hal/<概念>/inc/<概念>_esp32c3.h    平台具象派生类
#   hal/<概念>/src/*.c
#
# 抽象层头文件不包含任何 ESP-IDF / FreeRTOS 头，因此驱动依赖放 PRIV_REQUIRES，
# 使用方（main 等）只依赖 hal 的抽象头即可，不会被拖入平台头文件链。

file(GLOB_RECURSE src "*.c")

file(GLOB_RECURSE all_headers "*.h")
set(inc_dirs "")
foreach(header ${all_headers})
    get_filename_component(dir ${header} DIRECTORY)
    list(APPEND inc_dirs ${dir})
endforeach()
list(REMOVE_DUPLICATES inc_dirs)

idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS ${inc_dirs}
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_ledc esp_adc freertos
)
