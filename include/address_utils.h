//
// Created by user_wangzhen on 2026/5/13.
//

#ifndef SOHOOK_ADDRESS_UTILS_H
#define SOHOOK_ADDRESS_UTILS_H
#include <stdint.h>
// 获取模块加载基址
uintptr_t get_module_base(const char* module_name);

// 传入相对于 SO 的偏移，返回运行时内存绝对地址
uintptr_t get_absolute_address(const char* module_name, uintptr_t relative_addr);


#endif //SOHOOK_ADDRESS_UTILS_H