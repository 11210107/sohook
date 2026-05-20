cmake_minimum_required(VERSION 4.0)
project(sohook)

set(CMAKE_CXX_STANDARD 11)

# --- 关键修改点 1: 指定头文件搜索路径 ---
# 这会让编译器在编译 main.cpp 时自动搜索 include 目录
include_directories(include)
# 我们只用 main.cpp 里的逻辑生成库，它里面的 main 函数在库模式下会被忽略
#add_executable(sohook main.cpp)
#add_library(sohook SHARED src/main.cpp)



# 寻找日志库
find_library(log-lib log)
#target_link_libraries(sohook ${log-lib})

# 声明 Dobby 静态库
add_library(dobby STATIC IMPORTED)
# 设置静态库的具体路径（根据架构自动选择）
set_target_properties(dobby PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/thirdparty/dobby/arm64-v8a/libdobby.a
)
# 3. 生成 libsohook.so 插件库
# 包含了你的入口文件和核心 Hook 逻辑文件
add_library(sohook SHARED
        src/main.cpp                    # so入口
        src/address_utils.cpp           # 基础工具：基址获取
        src/protocol_utils.cpp          # 基础工具：PB解析
        src/file_utils.cpp          # 基础工具：file文件读写
        src/wework_logic_center.cpp     # 业务逻辑：Service 寻址 (新增)
        src/wework_message_factory.cpp  # 消息构建：Protobuf 模拟
        src/wework_conversation.cpp     # 会话构建：Protobuf 模拟
        src/wework_conversation_service.cpp     # 会话服务：消息发送
        src/wework_hook.cpp             # Hook 入口：拦截与触发
)

# 链接
target_link_libraries(sohook dobby ${log-lib})



# 注意：这里我们生成的是可执行文件 (Loader)
add_executable(my_loader loader.cpp)

# 在 Android/Linux 上使用 dlopen 需要链接 dl 库
target_link_libraries(my_loader dl)