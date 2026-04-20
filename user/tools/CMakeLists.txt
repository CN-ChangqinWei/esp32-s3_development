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
idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS ${inc_dirs}
    REQUIRES esp_driver_uart freertos mqtt
)
