cmake_minimum_required(VERSION 4.0)
project(sohook)

set(CMAKE_CXX_STANDARD 17)

# --- 关键修改点 1: 指定头文件搜索路径 ---
# 这会让编译器在编译 main.cpp 时自动搜索 include 目录
include_directories(include)
# 工程内头文件一律写"相对 src/"的路径（#include "native/conv_service.h"），
# 禁止 ../ 写法——目录重构时只需改一处目录名，不会出现半新半旧的引用
include_directories(${CMAKE_SOURCE_DIR}/src)
# protobuf/abseil 头文件路径
include_directories(${CMAKE_SOURCE_DIR}/thirdparty/protobuf/arm64-v8a/include)
# protobuf 生成的 .pb.h 文件路径
include_directories(${CMAKE_SOURCE_DIR}/proto/gen)

# 我们只用 main.cpp 里的逻辑生成库，它里面的 main 函数在库模式下会被忽略
#add_executable(sohook main.cpp)
#add_library(sohook SHARED src/main.cpp)



# 寻找日志库
find_library(log-lib log)
#target_link_libraries(sohook ${log-lib})
#  找到或者声明寻找系统 android 库
find_library(ANDROID_LIB android)
# 声明 Dobby 静态库
add_library(dobby STATIC IMPORTED)
# 设置静态库的具体路径（根据架构自动选择）
set_target_properties(dobby PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/thirdparty/dobby/arm64-v8a/libdobby.a
)

# 收集所有 abseil 静态库
file(GLOB absl_libs "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/arm64-v8a/lib/absl/*.a")

# 3. 生成 libsohook.so 插件库
#
# 源文件按目录自动收集：新增/移动文件不必再改本文件（CONFIGURE_DEPENDS 让
# 每次构建重新 glob，新文件下一轮 build 自动进来）。目录即层次，依赖方向单向：
#   hook → types → send → {message, native, media} → {config, runtime, utils}
#
#   src/main.cpp     so 入口（constructor 装 hook）
#   src/offset.h     原生偏移 + 内存布局单点（版本升级只动这里）
#   src/hook/        拦截面：nativeSendModelMessage 拦截与分发
#   src/native/      ABI 伪造层：服务寻址、会话/消息句柄、conv_type
#   src/message/     消息模型 PB（纯函数、零原生调用，可宿主单测）
#   src/send/        发送原语 send_model_message（全工程唯一发射口）
#   src/types/       一种消息类型一个文件（image/emotion/video/link/raw/db）
#   src/media/       CDN 票据缓存 + 上传 + 服务端换 URL
#   src/config/      *.req 触发配置文法与接收者解析
#   src/runtime/     主线程投递 executor、注入侧 dlopen hook
#   src/utils/       基址/文件/PB dump 等无业务语义工具
#   proto/gen/       protoc 生成的 .pb.cc
file(GLOB_RECURSE SOHOOK_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/proto/gen/*.cc
)

add_library(sohook SHARED ${SOHOOK_SOURCES})

# 链接
target_link_libraries(sohook dobby ${log-lib} ${ANDROID_LIB}
        ${CMAKE_SOURCE_DIR}/thirdparty/protobuf/arm64-v8a/lib/libprotobuf-lite.a
        ${CMAKE_SOURCE_DIR}/thirdparty/protobuf/arm64-v8a/lib/libprotobuf.a
        ${CMAKE_SOURCE_DIR}/thirdparty/protobuf/arm64-v8a/lib/libutf8_validity.a
        ${absl_libs}
)



# 注意：这里我们生成的是可执行文件 (Loader)
add_executable(my_loader loader.cpp)

# 在 Android/Linux 上使用 dlopen 需要链接 dl 库
target_link_libraries(my_loader dl)