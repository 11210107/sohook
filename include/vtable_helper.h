//
// Created by user_wangzhen on 2026/5/15.
//

#ifndef SOHOOK_VTABLE_HELPER_H
#define SOHOOK_VTABLE_HELPER_H

#include <stdint.h>
/**
 * 模拟 C++ 虚函数调用
 * @param instance 对象实例指针 (this)
 * @param index 虚表索引 (偏移量 / 8)
 * @return 虚函数执行后的返回值
 * 使用 inline 关键字防止在多个源文件中包含时产生重复定义错误
 */
static inline uintptr_t CallVirtualMethod(uintptr_t instance, int index) {
    if (!instance) return 0;

    // 1. 获取虚表指针 (对象的首 8 字节)
    uintptr_t* vtable = *(uintptr_t**)instance;

    // 2. 取出函数地址并执行
    typedef uintptr_t (*VirtualFunc)(uintptr_t);
    VirtualFunc func = (VirtualFunc)vtable[index];

    return func(instance);
}
#endif //SOHOOK_VTABLE_HELPER_H