# platform 组件：跨平台适配门面（内存 / 线程 / 时间戳 / 睡眠 / 同步）
#
# 说明：本仓库无根 CMakeLists，模块按 ESP-IDF component 方式被宿主工程消费。
#       本文件形态与同仓库 libs/services/CMakeLists.txt 保持一致，便于宿主工程
#       直接把 libs/platform 注册为组件（此前 platform 目录缺该文件，无法被编译）。

# 递归查找所有源文件
file(GLOB_RECURSE src "*.c")

# 递归查找所有包含头文件的目录
file(GLOB_RECURSE all_headers "*.h")
set(inc_dirs "")
foreach(header ${all_headers})
    get_filename_component(dir ${header} DIRECTORY)
    list(APPEND inc_dirs ${dir})
endforeach()
list(REMOVE_DUPLICATES inc_dirs)

# 注册组件
# REQUIRES 与 libs/services/CMakeLists.txt 对齐；platform 的实现层依赖
# thread_object / timestamp / freertos_extra / assert_marco / log。
# 若宿主工程中承载这些模块的组件名不同，请按宿主工程实际组件名调整本列表。
idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS ${inc_dirs}
    REQUIRES freertos tools robot
)
