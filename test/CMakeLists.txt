# ============================================================================
# hal_ext 抽象层单元测试工程（独立 ESP-IDF 工程，不参与主工程构建）
#
# 用法（IDE 终端，在仓库根目录执行）：
#   idf.py -C test set-target esp32c3      # 首次
#   idf.py -C test build flash monitor
#
# 说明：
#   - 只依赖 ../hal_ext（抽象父类）与 IDF 自带 unity 组件；
#   - 用例全部使用 fake 具象（不发真的 GPIO/UART/I2C/SPI/LEDC/ADC 访问），
#     因此不接线也能跑；
#   - esp32c3 的具象实现（*_esp32c3.c）需要真实外设，不在本工程的覆盖范围。
# ============================================================================
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../hal_ext")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_development_hal_tests)
