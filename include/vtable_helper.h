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
#endif //SOHOOK_VTABLE_HELPER_H