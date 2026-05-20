//
// Created by user_wangzhen on 2026/5/16.
//
#include <string>
#include "address_utils.h"
#include <vector>
#include <string>
#include "logger.h"
// 🛠️ 修复 1：引入 ARM NEON 向量头文件，解决 int64x2_t 报错
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#else
typedef struct { int64_t val[2]; } int64x2_t; // 兜底占位
#endif

// 映射 IDA 伪代码类型
typedef int64_t           __int64;
typedef uint64_t          _QWORD;
typedef uint32_t          _DWORD;
typedef unsigned char     _BYTE;

// 1. 根据你的逆向分析，定义底层纯 C++ 函数指针类型
typedef unsigned int * (*fn_sub_137E690)(int64_t *out_c_conv_ptr);

typedef int64x2_t (*fn_sub_1363110)(void *pb_struct_buffer);

typedef int64_t (*fn_sub_5F64AC4)(void *pb_struct_buffer, void *std_string_ptr);

typedef void (*fn_sub_1365C18)(int64_t internal_impl_ptr, void *pb_struct_buffer);

typedef void (*fn_sub_1363848)(void *pb_struct_buffer); // 析构清理函数
// 【新增】专门应对 ARM64 X8 调用约定的内联汇编包装函数
int64_t call_sub_137E690_via_x8(uintptr_t func_addr) {
    int64_t out_c_conv_ptr = 0;

    // 用内联汇编强行将 out_c_conv_ptr 的地址塞进 X8 寄存器，然后跳转
    asm volatile(
        "mov x8, %0\n"             // x8 = &out_c_conv_ptr
        "blr %1\n"                // 跳转到 sub_137E690
        :
        : "r"(&out_c_conv_ptr), "r"(func_addr)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x16", "x17", "lr", "memory"
    );

    return out_c_conv_ptr; // 返回内部 operator new(0x168) 出来的真实对象指针
}
// NEON 返回值专用包装（第一个参数直接改用 fn_sub_1363110 类型，省去强转）
void safe_init_pb_struct(fn_sub_1363110 func_ptr, void* pb_buffer_ptr) {
    asm volatile(
        "mov x0, %0\n"
        "blr %1\n"
        :
        : "r"(pb_buffer_ptr), "r"(func_ptr)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x16", "x17", "v0", "v1", "v2", "v3", "lr", "memory"
    );
}
void* create_and_inject_conversation() {
    LOGI("[CreateConversation] start");
    // 获取基址并计算函数绝对地址
    unsigned long long base = get_module_base("libwework_framework.so");
    if (!base) {
        LOGE("[CreateConversation] 错误: 找不到目标 SO 基址");
        return nullptr;
    }
    // 绑定函数绝对地址
    uintptr_t create_cpp_empty_obj_addr = base + 0x137E690;
    auto init_pb_struct = (fn_sub_1363110) (base + 0x1363110);
    auto parse_from_mem = (fn_sub_5F64AC4) (base + 0x5F64AC4);
    auto sync_to_conversation = (fn_sub_1365C18) (base + 0x1365C18);
    auto destory_pb_struct = (fn_sub_1363848) (base + 0x1363848);
    // ==========================================
    // 第一步：在 C++ 堆上分配底层的真实 Conversation 对象
    // ==========================================
    // 【修改】舍弃原有的普通函数指针调用，改用汇编包装函数
    int64_t my_c_conv_ptr = call_sub_137E690_via_x8(create_cpp_empty_obj_addr);

    if (!my_c_conv_ptr) {
        LOGE("[CreateConversation] 错误: 无法创建 my_c_conv_ptr (X8 传参失败)");
        return nullptr;
    }
    LOGI("[CreateConversation] 成功通过 X8 获取对象指针: 0x%lx", my_c_conv_ptr);

    // 2. 强行 16 字节对齐分配缓冲区（迎合 OWORD 128位清理指令）
    // 分配 40 个 uintptr_t = 320 字节，足够容纳（最大偏移 288 + 4）
    // 2. 强行 16 字节对齐分配原生数组（320 字节）
    alignas(16) uintptr_t pb_buffer[40] = {0};

    // 💡 修正点：原生数组 pb_buffer 本身就可以当指针用，直接传入
    safe_init_pb_struct(init_pb_struct, pb_buffer);
    LOGI("[CreateConversation] Protobuf 结构体安全初始化成功");

    // 3. 准备 PB 原始数据
    unsigned char raw_proto[] = {
        0x08, 0x97, 0xA0, 0x83, 0xA4, 0xD1, 0xC9, 0xF0, 0xAE, 0x69,
        0x10, 0xEC, 0xAC, 0x95, 0xF8, 0x80, 0x80, 0x80, 0x0E, 0x60,
        0x00, 0x7A, 0x00
    };
    std::string cpp_str_stream((char *)raw_proto, 23);

    // ==========================================
    // 第四步：在纯内存层解析 Protobuf 数据
    // ==========================================
    // 4. 解析
    // 💡 修正点：去掉所有 .data()，直接传入 pb_buffer
    uintptr_t parse_ret = parse_from_mem(pb_buffer, &cpp_str_stream);
    LOGI("[CreateConversation] pb 解析返回值: %lu", parse_ret);

    if (parse_ret == 1 == 1LL) { // 成功分支
        // 5. 寻址同步
        uintptr_t internal_impl = *reinterpret_cast<uintptr_t*>(my_c_conv_ptr + 112LL);
        if (!internal_impl) {
            LOGE("[CreateConversation] internal_impl 为空");
            destory_pb_struct(pb_buffer);
            return nullptr;
        }

        // 调用同步
        // 💡 修正点：统一传入 pb_buffer
        sync_to_conversation(internal_impl, pb_buffer);
        LOGI("[CreateConversation] 🎉 完美创建并注入会话: 0x%lx", my_c_conv_ptr);
    } else {
        LOGE("[CreateConversation] 错误: parse_ret 解析 pb 失败");

        // 避坑：在这里如果失败了，应该处理 my_c_conv_ptr 悬挂指针的释放，暂返回 0
        destory_pb_struct(pb_buffer);
        return nullptr;
    }
    // ==========================================
    // 第六步：清理栈上的临时变量，防止内存泄漏
    // ==========================================
    destory_pb_struct(pb_buffer);
    // 【最终战果】
    // 此时的 my_c_conv_ptr 指针就是一个完整体 C++ Conversation 对象了！
    // 你可以拿着这个 64 位指针直接调用底层的 C++ 发消息、发文件等核心 Native 方法！
    // ==========================================
    return (void*)my_c_conv_ptr;
}