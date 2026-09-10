//
// Created by user_wangzhen on 2026/7/27.
//

#pragma once

#include <cstdint>

#define OFFSET_JAVA_VM                      (0x92520D8) // java_vm

#define OFFSET_NATIVE_GET_CURRENT_PROFILE   (0x1349A90)  // nativeGetCurrentProfile
#define OFFSET_NATIVE_VID                   (0x11E3DC8) // nativeVid

#define OFFSET_NATIVE_MSG_SEND              (0x10E79F8) // 消息发送 nativeSendModelMessage
#define OFFSET_MSG_SEND                     (0x24613B0) // 消息发送
#define OFFSET_UNWRAP_NATIVE_HANDLE         (0xF9D90C)  // currentProfile 解引用
#define OFFSET_MSG_SEND_CB                  (0x6D35F60) // 消息发送闭包回调
#define OFFSET_NATIVE_GET_SERVICE_MANAGER   (0x1348264) // nativeGetServiceManager
#define OFFSET_NATIVE_GET_CONV_SERVICE      (0x13B3068) // nativeGetConversationService
#define OFFSET_ADD_REF                      (0x6D4E100) // 引用增加
#define OFFSET_DEC_REF                      (0x6D4E0E8) // 引用减少
#define OFFSET_CREATE_CONV                  (0x151D6C0) // 创建空会话
#define OFFSET_INIT_PB                      (0x1596B20) // 初始化PB
#define OFFSET_PARSE_FROM_MEM               (0x6DFB758) // 从内存解析
#define OFFSET_SYNC_TO_CONV                 (0x159AAC0) // 同步到会话
#define OFFSET_DESTROY_PB_STRUCT            (0x1597A88) // 销毁PB结构体
#define OFFSET_CREATE_MSG_PTR               (0x151D7F4) // 创建消息指针
#define OFFSET_FIND_CONV_BY_CACHE           (0x50CE9D0) // 根据会话key查找会话
#define OFFSET_GET_PROFILE_MS               (0x23D6F28) // 获取 ProfileManagerService 在ProfileManager_nativeGetCurrentProfile



// ===== native 句柄对象内存布局（Message/Conversation 同构；版本升级时只需核对此处）=====
#define OFFSET_HANDLE_REF_COUNT             (96)    // uint32 侵入式引用计数器，适配位置在 OFFSET_DEC_REF 方法入参。
#define OFFSET_HANDLE_IMPL                  (120)   // internal_impl 核心指针，适配位置在 OFFSET_CREATE_CONV 方法中，处理operator new(296LL);分配内存
#define OFFSET_CONVERSATION_ID              (200)   // 192 + 8  *(_QWORD *)(v5 + 192) = v2;
#define OFFSET_HANDLE_VTABLE_DTOR           (8)     // vtable 内 deleting destructor 偏移，适配位置在 nativeSendModelMessage方法中，处理delete this;释放内存
#define OFFSET_MSG_LOCAL_ID                 (64)   // msg impl 内 local_id，适配位置在 OFFSET_CREATE_MSG_PTR 方法中
#define OFFSET_IMPL_HAS_BITS                (16)    // msg/conv impl 内 has_bits，适配位置在 OFFSET_CREATE_CONV 方法中
#define OFFSET_CONV_LOCAL_ID                (192)   // conv impl 内 localId，适配位置在 OFFSET_CREATE_CONV 方法中
#define OFFSET_CONV_SERVICE_MAP_HOLDER      (288)   // ConversationService 内会话缓存 map_holder 指针，适配位置在 OFFSET_FIND_CONV_BY_CACHE 方法中

#define OFFSET_MAP_HOLDER_ROOT              (112)   // map_holder 内红黑树根节点，适配位置在 OFFSET_FIND_CONV_BY_CACHE 方法中
#define OFFSET_PROFILE_MANAGER              (24)    // ProfileManager_nativeGetCurrentProfile   v1 = (*(__int64 (__fastcall **)(unsigned __int64))(*(_QWORD *)v0 + 24LL))(v0);
#define OFFSET_CURRENT_PROFILE              (24)    // ProfileManager_nativeGetCurrentProfile   result = (_QWORD *)(*(__int64 (__fastcall **)(__int64))(*(_QWORD *)v1 + 24LL))(v1);
#define OFFSET_SERVICE_MANAGER              (264)   // Profile_nativeGetServiceManager
#define OFFSET_CONTACT_SERVICE              (96)    // ContactService_nativeIsContactAdded
#define OFFSET_GET_CACHE_CONV               (520)   // ConversationService_nativeGetCacheConversationByKey

// 取句柄 OFFSET_HANDLE_REF_COUNT 处的 32 位引用计数器
inline uint32_t *handle_ref_count(void *handle) {
    return reinterpret_cast<uint32_t *>(
        reinterpret_cast<char *>(handle) + OFFSET_HANDLE_REF_COUNT);
}

// 取句柄 OFFSET_HANDLE_IMPL 处的 internal_impl 核心指针
inline void *handle_impl(void *handle) {
    return *reinterpret_cast<void **>(reinterpret_cast<char *>(handle) + OFFSET_HANDLE_IMPL);
}

// 计数归零时调 vtable + OFFSET_HANDLE_VTABLE_DTOR 的 deleting destructor
inline void handle_destroy(void *handle) {
    (*reinterpret_cast<void (**)(void *)>(
        *reinterpret_cast<uintptr_t *>(handle) + OFFSET_HANDLE_VTABLE_DTOR))(handle);
}
