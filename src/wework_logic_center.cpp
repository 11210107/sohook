//
// Created by user_wangzhen on 2026/5/15.
//
#include "address_utils.h"
#include "vtable_helper.h"
#include "wework_message_factory.h"
#include "logger.h"
#include <cstring>
// 虚表中的函数索引（根据汇编：+24LL 对应 index 3，+264LL 对应 index 33）
#define VTABLE_INDEX_GET_LOGIC 33    // 264 / 8 = 33
#define VTABLE_INDEX_GET_CONV_SVC 13  // 104 / 8 = 13


// 定义单例获取函数类型
typedef uintptr_t (*GetSingletonMethod)();

uintptr_t getCurrentConvService() {
    // 1. 获取 ProfileManager 内部单例获取函数的绝对地址
    // sub_2477954 是 libwework_framework.so 里的地址
    static auto GetProfileMgrInstance = (GetSingletonMethod) get_absolute_address("libwework_framework.so", 0x2477954);
    if (!GetProfileMgrInstance) {
        LOGE("[getCurrentConvService] 错误: 找不到 GetProfileMgrInstance (0x2477954)");
        return 0;
    }
    // 2. 执行 v0 = sub_2477954()
    uintptr_t profile_mgr_instance = GetProfileMgrInstance();
    if (!profile_mgr_instance) {
        LOGE("[getCurrentConvService] 错误: profile_mgr_instance 为空");
        return 0;
    }
    LOGI("[getCurrentConvService] ProfileManager Instance: %p", (void*)profile_mgr_instance);
    // 3. 获取 ActiveProfileManager (v1) - 虚表偏移 24 (Index 3)
    uintptr_t active_mgr = CallVirtualMethod(profile_mgr_instance, 3);
    if (!active_mgr) {
        LOGE("[getCurrentConvService] 错误: 无法获取 active_mgr (vtable index 3)");
        return 0;
    }
    LOGI("[getCurrentConvService] ActiveProfileManager: %p", (void*)active_mgr);

    // 4. 获取真正的 Profile 对象指针 - 虚表偏移 24 (Index 3)
    uintptr_t profile_obj = CallVirtualMethod(active_mgr, 3);
    if (!profile_obj) {
        LOGE("[getCurrentConvService] 错误: 无法获取 profile_obj (vtable index 3)");
        return 0;
    }
    LOGI("[getCurrentConvService] Profile Object: %p", (void*)profile_obj);
    // 5. 获取 Logic 对象 (v11) - 虚表偏移 264 (Index 33)
    uintptr_t logic_obj = CallVirtualMethod(profile_obj, 33);
    if (!logic_obj) {
        LOGE("[getCurrentConvService] 错误: 无法获取 logic_obj (vtable index 33)");
        return 0;
    }
    LOGI("[getCurrentConvService] Logic Object: %p", (void*)logic_obj);
    // 6. 获取 ConversationService (v12) - 虚表偏移 104 (Index 13)
    uintptr_t conv_service = CallVirtualMethod(logic_obj, 13);
    if (!conv_service) {
        LOGE("[getCurrentConvService] 错误: 无法获取 conv_service (vtable index 13)");
        return 0;
    }
    return conv_service; // 这就是 v10，不需要那个 16 字节的句柄包装了
}


