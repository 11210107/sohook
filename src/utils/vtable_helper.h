//
// vtable_helper.h — 手写虚表调用的通用助手（CallVirtualMethod / CallVMethodByOffset）
//
// 变更（v2，随 gif_url_exchange 补丁）：gif_url_exchange.cpp 是工程内首个使用
// CallVMethodByOffset<void>(...)（无返回值虚调用）的模块，原实现的守卫路径
// `return Ret{};` 在 Ret=void 时非法（clang: illegal initializer type 'void'），
// Android 构建同样报错——host_test 宿主编译首先捕获。修复：新增
// vtable_call_default<Ret>() + void 显式特化（C++11 兼容，不依赖 if constexpr）。
//
// 落位：src/utils/vtable_helper.h（原在工程根 include/，随 include/ 瘦身迁入 src；
// 纯工具、不含任何企微偏移知识——内存布局单点仍是 src/offset.h）。
//
// Created by user_wangzhen on 2026/5/15.
//

#ifndef SOHOOK_VTABLE_HELPER_H
#define SOHOOK_VTABLE_HELPER_H

#include <stdint.h>
#include <cstddef>
/**
 * 模拟 C++ 虚函数调用
 * @param instance 对象实例指针 (this)
 * @param index 虚表索引 (偏移量 / 8)
 * @return 虚函数执行后的返回值
 * 使用 inline 关键字防止在多个源文件中包含时产生重复定义错误
 */
static inline uintptr_t CallVirtualMethod(uintptr_t instance, int index) {
    if (!instance) return 0;

    /**
    * 等价写法
    * void** vtable_ptr_location = (void**)instance; // 1. 先把整数地址，转成指向“指针”的指针（本质就是转化为符合当前系统位数的指针类型）
    * void* vtable_address = *vtable_ptr_location; // 2. 解引用，取出里面存放的虚表地址
    * uintptr_t* vtable = (uintptr_t*)vtable_address; // 3. 转换为数组指针准备后续索引
    */
    // 1. 获取虚表指针 (对象的首 8 字节)
    uintptr_t* vtable = *(uintptr_t**)instance;

    // 2. 取出函数地址并执行
    typedef uintptr_t (*VirtualFunc)(uintptr_t);
    VirtualFunc func = (VirtualFunc)vtable[index];

    return func(instance);
}

/**
 * @brief 虚调用失败时的默认返回（Ret=void 显式特化：Ret{} 对 void 非法；
 *        gif_url_exchange.cpp 首次用到 Ret=void 无返回值虚调用）
 */
template <typename Ret>
inline Ret vtable_call_default() { return Ret{}; }

template <>
inline void vtable_call_default<void>() {}

/**
 * @brief 通过字节偏移（Offset）调用虚函数
 * @tparam Ret 返回值类型，默认为 uintptr_t
 * @tparam Args 参数类型包
 * @param instance 对象实例指针
 * @param offset_bytes IDA / 反编译代码中的字节偏移量（例如 1032LL、264LL）
 * @param args 传递给虚函数的实参
 */
template <typename Ret = uintptr_t, typename... Args>
inline Ret CallVMethodByOffset(uintptr_t instance, size_t offset_bytes, Args... args) {
    if (!instance || (instance % 8 != 0)) return vtable_call_default<Ret>();

    // 1. 读取对象的首 8 字节获取 vtable 地址
    auto vtable = *reinterpret_cast<uintptr_t**>(instance);
    if (!vtable || (reinterpret_cast<uintptr_t>(vtable) % 8 != 0)) return vtable_call_default<Ret>();

    // 2. 将字节偏移转为虚表数组索引 (64 位下为 offset / 8)
    auto func_ptr = reinterpret_cast<Ret(*)(uintptr_t, Args...)>(vtable[offset_bytes / 8]);
    if (!func_ptr) return vtable_call_default<Ret>();

    // 3. 执行虚函数调用
    return func_ptr(instance, args...);
}

/**
 * @brief 通过虚表索引（Index）调用虚函数
 */
template <typename Ret = uintptr_t, typename... Args>
inline Ret CallVMethodByIndex(uintptr_t instance, size_t index, Args... args) {
    return CallVMethodByOffset<Ret>(instance, index * sizeof(uintptr_t), args...);
}
#endif //SOHOOK_VTABLE_HELPER_H
