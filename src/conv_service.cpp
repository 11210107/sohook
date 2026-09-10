//
// Created by user_wangzhen on 2026/9/9.
//
#include <cstdint>
#include "conv_service.h"
#include <cinttypes>
#include "utils/address_utils.h"
#include "../include/logger.h"
#include "offset.h"

uintptr_t getConversationService() {
    auto get_current_profile = (uint64_t (*)(uint64_t, uint64_t)) get_abs_addr(
        "libwework_framework.so", OFFSET_NATIVE_GET_CURRENT_PROFILE);

    auto get_service_manager = (uint64_t (*)(uint64_t, uint64_t, uint64_t)) get_abs_addr(
        "libwework_framework.so", OFFSET_NATIVE_GET_SERVICE_MANAGER);
    auto get_conversation_service = (uint64_t (*)(uint64_t, uint64_t, uint64_t)) get_abs_addr(
        "libwework_framework.so", OFFSET_NATIVE_GET_CONV_SERVICE);
    if (!get_current_profile) {
        LOGE("get_vid: resolve OFFSET_NATIVE_GET_CURRENT_PROFILE failed");
        return 0;
    }

    uintptr_t profile_wrapper = get_current_profile(0, 0);
    if (!profile_wrapper) {
        LOGE("profile_wrapper null");
        return 0;
    }

    uintptr_t service_mgr = get_service_manager(0, 0, profile_wrapper);
    if (!service_mgr) {
        LOGE("service_mgr null");
        return 0;
    }
    operator delete(reinterpret_cast<void *>(profile_wrapper));

    uintptr_t conv_service = get_conversation_service(0, 0, service_mgr);
    if (!conv_service) {
        LOGE("conv_service null");
        return 0;
    }

    return conv_service;
}

struct alignas(8) ConversationKey {
    int32_t type;       // conv_type
    uint32_t padding;   // 对齐
    uint64_t remote_id; // remote_id
    uint64_t sub_id;    // 设为 0 (对应你的 reserved/sub_id)
};

// 2. 24 字节的大结构体，用于强迫 ARM64 编译器将接收地址放入 X8 寄存器
struct NativeResultHolder {
    uintptr_t conv_ptr; // X8 指向的第一个 QWORD，即 *a3 保存的 Conversation 指针
    uintptr_t unused1;
    uintptr_t unused2;
};

// 3. 虚表包装函数
void* get_cache_conversation_by_key(uint32_t conv_type, uint64_t remote_id) {
    // 1. 获取 ConversationService 指针
    uintptr_t conv_service = getConversationService();
    if (!conv_service) {
        LOGE("get_cache_conv: conv_service is null");
        return 0;
    }

    // 2. 构造 Key 参数 (a2)
    ConversationKey key{};
    key.type = static_cast<int32_t>(conv_type);
    key.padding = 0;
    key.remote_id = remote_id;
    key.sub_id = 0;

    // 3. 读取虚表 (0x208 = 520LL)
    uintptr_t vtable = *reinterpret_cast<uintptr_t *>(conv_service);
    uintptr_t func_ptr = *reinterpret_cast<uintptr_t *>(vtable + OFFSET_GET_CACHE_CONV);
    // 4. 定义函数签名：
    // 在 ARM64 ABI 中，当返回类型为 NativeResultHolder（>16字节）时：
    // 编译器会自动将 &holder 放到 X8 寄存器，service_ptr 放到 X0，key_ptr 放到 X1
    using GetCacheConvFn = NativeResultHolder (*)(uintptr_t service_ptr, const ConversationKey *key_ptr);
    auto get_cache_conv_func = reinterpret_cast<GetCacheConvFn>(func_ptr);

    // 5. 调用虚表函数，X8 寄存器将自动指向 holder 的内存区域
    NativeResultHolder holder = get_cache_conv_func(conv_service, &key);

    uintptr_t conversation_ptr = holder.conv_ptr;

    if (!conversation_ptr) {
        LOGW("get_cache_conv: conversation not found for remote_id: %llu", remote_id);
        return nullptr;
    }

    LOGD("Successfully got cached Conversation: 0x%lX", conversation_ptr);
    return reinterpret_cast<void*>(conversation_ptr);
}


