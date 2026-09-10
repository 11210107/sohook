//
// Created by user_wangzhen on 2026/9/10.
//
#include "utils/address_utils.h"
#include "offset.h"
#include "logger.h"
#include "contact_service.h"
#include "vtable_helper.h"

uintptr_t getContactService() {
    // 1. 获取 ProfileManager 单例实例
    static auto getAppInstance = reinterpret_cast<uintptr_t(*)()>(
        get_absolute_address("libwework_framework.so", OFFSET_GET_PROFILE_MS)
    );
    if (!getAppInstance) {
        LOGE("[+] 错误: 找不到 GetAppInstance");
        return 0;
    }

    uintptr_t pApp = getAppInstance();
    if (!pApp) return 0;

    // 2~5. 链式/层级虚函数调用
    uintptr_t pProfileMgr = CallVMethodByOffset(pApp, OFFSET_PROFILE_MANAGER);
    if (!pProfileMgr) return 0;

    uintptr_t pProfile = CallVMethodByOffset(pProfileMgr, OFFSET_CURRENT_PROFILE);
    if (!pProfile) return 0;

    uintptr_t pServiceMgr = CallVMethodByOffset(pProfile, OFFSET_SERVICE_MANAGER);
    if (!pServiceMgr) return 0;

    uintptr_t pContactService = CallVMethodByOffset(pServiceMgr, OFFSET_CONTACT_SERVICE);
    if (!pContactService) {
        LOGE("[+] 错误: GetContactService 返回空指针");
        return 0;
    }

    LOGD("[+] 成功获取 ContactService 指针: 0x%lX", pContactService);
    return pContactService;
}

bool isContactAdded(uint64_t remote_id) {
    LOGD("isContactAdded %llu", remote_id);
    uintptr_t pContactService = getContactService();
    if (!pContactService) return false;
    // 直接使用偏移 1032LL 调用 isContactAdded
    auto res = CallVMethodByOffset<uint64_t>(pContactService, 1032, remote_id);
    return (res & 1) != 0;
}


