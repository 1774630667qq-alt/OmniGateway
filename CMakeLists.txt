cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(OmniGateway)

# 设置 C++ 标准为 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启多线程支持
find_package(Threads REQUIRED)

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/include)
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party)
    include_directories(${PROJECT_SOURCE_DIR}/third_party)
endif()

# 收集所有源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")
# 移除包含自己 main 函数的示例文件
list(REMOVE_ITEM SOURCES "${PROJECT_SOURCE_DIR}/src/epollimpl.cpp")

# 编译为可执行文件
add_executable(OmniGateway ${SOURCES})

# 链接多线程库
target_link_libraries(OmniGateway PRIVATE Threads::Threads)

# 将项目根目录路径注入为编译期宏，供 SSL 证书等资源定位使用
target_compile_definitions(OmniGateway PRIVATE PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
