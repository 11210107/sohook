//
// Created by user_wangzhen on 2026/5/13.
//

#include "address_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <mutex>
#include <string>
#include "logger.h"

// 使用 map 缓存基址，避免重复读取 maps 文件
static std::map<std::string, uintptr_t> g_base_cache;

static std::mutex g_base_cache_lock; // 增加互斥锁保护并发安全

uintptr_t get_module_base(const char *module_name) {
    if (!module_name) {
        return 0;
    }
    // 1. 尝试从缓存中读取
    {
        std::lock_guard<std::mutex> lock(g_base_cache_lock);
        auto it = g_base_cache.find(module_name);
        if (it != g_base_cache.end() && it->second != 0) {
            return it->second;
        }
    }
    // 2. 打开 /proc/self/maps 实时读取
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (!fp) {
        return 0;
    }
    uintptr_t addr = 0;
    char line[1024] = {0};
    int line_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_count++;
        if (strstr(line, module_name)) {
            // 解析 64 位内存地址
            addr = strtoull(line, nullptr, 16);
            LOGI("[BASE] 🎯 在 maps 第 %d 行匹配成功!", line_count);
            LOGI("[BASE] RAW Line: %s", line); // 打印匹配到的完整映射行
            LOGI("[BASE] ✅ 解析得到基址: %s -> %p", module_name, (void *)addr);
            // 写入缓存
            if (addr != 0) {
                std::lock_guard<std::mutex> lock(g_base_cache_lock);
                g_base_cache[module_name] = addr;
                LOGI("[BASE] 💾 成功将 %s 的基址 %p 写入缓存", module_name, (void *)addr);
            }
            break;
        }
    }
    fclose(fp);
    if (addr == 0) {
        LOGD("[BASE] 🔍 未找到模块: %s (已扫描 %d 行)", module_name, line_count);
    }
    return addr;
}

bool is_so_loaded(const char *soname) {
    return get_module_base(soname) != 0;
}

uintptr_t get_absolute_address(const char *module_name, uintptr_t relative_addr) {
    uintptr_t base = get_module_base(module_name);
    if (base == 0) return 0;
    return base + relative_addr;
}

void *get_abs_addr(const char *soname, uintptr_t offset) {
    return reinterpret_cast<void *>(get_absolute_address(soname, offset));
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

