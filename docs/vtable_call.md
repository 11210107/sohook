```
uintptr_t getContactService() {
    // 1. 获取 ProfileManager 单例实例
    static auto getProfileMgrInstance = reinterpret_cast<uintptr_t(*)()>(
        get_absolute_address("libwework_framework.so", OFFSET_GET_PROFILE_MS)
    );
    if (!getProfileMgrInstance) {
        LOGE("[+] 错误: 找不到 GetProfileMgrInstance");
        return 0;
    }
    uintptr_t pProfileMgr = getProfileMgrInstance();
    if (!pProfileMgr) {
        LOGE("[+] 错误: ProfileManager 实例为空");
        return 0;
    }
    // 2. ProfileManager -> GetActiveProfileManager() (字节偏移 OFFSET_PROFILE_MANAGER)
    uintptr_t vtbl_profileMgr = *reinterpret_cast<uintptr_t *>(pProfileMgr);
    auto getActiveProfileMgr = reinterpret_cast<uintptr_t(*)(uintptr_t self)>(
        *reinterpret_cast<uintptr_t *>(vtbl_profileMgr + OFFSET_PROFILE_MANAGER)
    );
    uintptr_t activeMgr = getActiveProfileMgr(pProfileMgr);
    if (!activeMgr) {
        LOGE("[+] 错误: ActiveProfileManager 为空");
        return 0;
    }
    // 3. ActiveProfileManager -> GetCurrentProfile() (字节偏移 OFFSET_CURRENT_PROFILE)
    uintptr_t vtbl_activeMgr = *reinterpret_cast<uintptr_t *>(activeMgr);
    auto getCurrentProfile = reinterpret_cast<uintptr_t(*)(uintptr_t self)>(
        *reinterpret_cast<uintptr_t *>(vtbl_activeMgr + OFFSET_CURRENT_PROFILE)
    );
    uintptr_t pProfile = getCurrentProfile(activeMgr);
    if (!pProfile) {
        LOGE("[+] 错误: Profile 指针为空");
        return 0;
    }
    // 4. Profile -> GetServiceManager() (字节偏移 OFFSET_SERVICE_MANAGER)
    uintptr_t vtbl_profile = *reinterpret_cast<uintptr_t *>(pProfile);
    auto getServiceManager = reinterpret_cast<uintptr_t(*)(uintptr_t self)>(
        *reinterpret_cast<uintptr_t *>(vtbl_profile + OFFSET_SERVICE_MANAGER)
    );
    uintptr_t pServiceMgr = getServiceManager(pProfile);
    if (!pServiceMgr) {
        LOGE("[+] 错误: GetServiceManager 返回空指针");
        return 0;
    }
    // 5. ServiceManager -> GetContactService() (字节偏移 OFFSET_CONTACT_SERVICE)
    uintptr_t vtbl_serviceMgr = *reinterpret_cast<uintptr_t *>(pServiceMgr);
    auto getContactServiceFunc = reinterpret_cast<uintptr_t(*)(uintptr_t self)>(
        *reinterpret_cast<uintptr_t *>(vtbl_serviceMgr + OFFSET_CONTACT_SERVICE)
    );
    uintptr_t pContactService = getContactServiceFunc(pServiceMgr);
    LOGD("[+] 成功获取 ContactService 指针: 0x%lX", pContactService);
    if (!pContactService) {
        LOGE("[+] 错误: GetContactService 返回空指针");
        return 0;
    }
    return pContactService;
}

bool isContactAdded(uint64_t remote_id) {
    LOGD("isContactAdded %llu", remote_id);
    uintptr_t pContactService = getContactService();
    LOGD("pContactService %lX", pContactService);
    if (!pContactService) {
        return false;
    }
    // 1. 获取虚表指针 (vtable)
    uintptr_t *vtable = *reinterpret_cast<uintptr_t **>(pContactService);
    if (!vtable || (reinterpret_cast<uintptr_t>(vtable) % 8 != 0)) {
        return false;
    }
    // 2. 对应 IDA 中的 (+1032LL)，在 64 位系统下索引为 1032 / 8 = 129
    using IsAddedFn = uint64_t (*)(uintptr_t self, uint64_t remote_id);
    auto fn = reinterpret_cast<IsAddedFn>(vtable[129]);
    if (!fn) return false;
    // 3. 执行虚函数调用
    uint64_t res = fn(pContactService, remote_id);
    LOGD("isContactAdded result: %lld", res);
    return (res & 1) != 0;
}
```


### ConversationService
```
/*
uintptr_t getConversationService() {
    // 1. 获取 nativeGetCurrentProfile 函数指针
    typedef uintptr_t (*nativeGetCurrentProfile_t)(uintptr_t env, uintptr_t clazz);
    auto get_current_profile = reinterpret_cast<nativeGetCurrentProfile_t>(
        get_abs_addr("libwework_framework.so", OFFSET_NATIVE_GET_CURRENT_PROFILE)
    );

    if (!get_current_profile) {
        LOGE("resolve OFFSET_NATIVE_GET_CURRENT_PROFILE failed");
        return 0;
    }

    // 2. 调用 nativeGetCurrentProfile 获取 16字节 Profile Wrapper 指针 (a3)
    uintptr_t profile_wrapper = get_current_profile(0, 0);
    if (!profile_wrapper) {
        LOGE("profile_wrapper is null");
        return 0;
    }

    // 3. 解引用获取 Profile 真实 C++ 对象指针 (sub_F9D90C 逻辑)
    uintptr_t real_profile = *reinterpret_cast<uintptr_t*>(profile_wrapper);
    if (!real_profile) {
        LOGE("real_profile is null");
        return 0;
    }

    // 4. 沿着虚表寻址: Profile (vtable + 264 / 0x108) -> ServiceManager (v27)
    uintptr_t profile_vtable = *reinterpret_cast<uintptr_t*>(real_profile);
    typedef uintptr_t (*get_service_mgr_t)(uintptr_t profile_ptr);
    auto get_service_mgr = reinterpret_cast<get_service_mgr_t>(
        *reinterpret_cast<uintptr_t*>(profile_vtable + 264)
    );

    uintptr_t service_mgr = get_service_mgr(real_profile);
    if (!service_mgr) {
        LOGE("service_mgr is null");
        return 0;
    }

    // 5. 沿着虚表寻址: ServiceManager (vtable + 40 / 0x28) -> ConversationService (v28)
    uintptr_t service_mgr_vtable = *reinterpret_cast<uintptr_t*>(service_mgr);
    typedef uintptr_t (*get_conv_service_t)(uintptr_t service_mgr_ptr);
    auto get_conv_service = reinterpret_cast<get_conv_service_t>(
        *reinterpret_cast<uintptr_t*>(service_mgr_vtable + 40)
    );

    uintptr_t conv_service = get_conv_service(service_mgr);
    if (!conv_service) {
        LOGE("conv_service is null");
        return 0;
    }

    // 注意：无需手动 delete profile_wrapper，交由底层垃圾回收机制或让其在 Native 生命周期内自然留存

    LOGI("Successfully acquired ConversationService (v28): 0x%" PRIxPTR, conv_service);
    return conv_service;
}
*/
```


