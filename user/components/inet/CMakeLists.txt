# 设置源文件
file(GLOB_RECURSE src "src/*.c")
# 设置头文件目录 
set(inc "inc")

# 注册组件
idf_component_register(
    SRCS ${src}
    INCLUDE_DIRS ${inc}
    REQUIRES esp_wifi
)

