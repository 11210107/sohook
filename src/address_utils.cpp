//
// Created by user_wangzhen on 2026/5/13.
//

#include "address_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include "logger.h"

// 使用 map 缓存基址，避免重复读取 maps 文件
static std::map<std::string, uintptr_t> g_base_cache;

uintptr_t get_module_base(const char *module_name) {
    if (g_base_cache.count(module_name)) return g_base_cache[module_name];

    uintptr_t addr = 0;
    char line[1024];
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, module_name)) {
                addr = strtoul(line, nullptr, 16);
                g_base_cache[module_name] = addr; // 缓存结果
                break;
            }
        }
        fclose(fp);
    }
    return addr;
}

uintptr_t get_absolute_address(const char *module_name, uintptr_t relative_addr) {
    uintptr_t base = get_module_base(module_name);
    if (base == 0) return 0;
    return base + relative_addr;
}



// 2. 实现 CallVirtualMethod (必须放在调用它的函数之前)
/**
 * 模拟 C++ 虚函数调用
 * @param instance 对象实例指针 (this)
 * @param index 虚表索引 (偏移量 / 8)
 * @return 虚函数执行后的返回值
 */
static uintptr_t CallVirtualMethod(uintptr_t instance, int index) {
    if (!instance) return 0;
    // 对象首地址即虚表指针
    uintptr_t* vtable = *(uintptr_t**)instance;
    // 取得虚函数并执行，传入实例本身作为第一个参数 (x0)
    typedef uintptr_t (*VirtualFunc)(uintptr_t);
    VirtualFunc func = (VirtualFunc)vtable[index];
    return func(instance);
}

