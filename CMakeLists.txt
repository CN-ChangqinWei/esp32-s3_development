# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 添加自定义组件目录
set(EXTRA_COMPONENT_DIRS user/components user/tools user/tools/serial_proto user/services user/robot)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_development)
