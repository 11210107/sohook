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
 * @brief 通过字节偏移（Offset）调用虚函数
 * @tparam Ret 返回值类型，默认为 uintptr_t
 * @tparam Args 参数类型包
 * @param instance 对象实例指针
 * @param offset_bytes IDA / 反编译代码中的字节偏移量（例如 1032LL、264LL）
 * @param args 传递给虚函数的实参
 */
template <typename Ret = uintptr_t, typename... Args>
inline Ret CallVMethodByOffset(uintptr_t instance, size_t offset_bytes, Args... args) {
    if (!instance || (instance % 8 != 0)) return Ret{};

    // 1. 读取对象的首 8 字节获取 vtable 地址
    auto vtable = *reinterpret_cast<uintptr_t**>(instance);
    if (!vtable || (reinterpret_cast<uintptr_t>(vtable) % 8 != 0)) return Ret{};

    // 2. 将字节偏移转为虚表数组索引 (64 位下为 offset / 8)
    auto func_ptr = reinterpret_cast<Ret(*)(uintptr_t, Args...)>(vtable[offset_bytes / 8]);
    if (!func_ptr) return Ret{};

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