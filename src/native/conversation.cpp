//
// Created by user_wangzhen on 2026/9/9.
//
#include "native/conversation.h"
#include "utils/address_utils.h"
#include "utils/logger.h"
#include "offset.h"
#include <string>

#include "utils/protocol_utils.h"
#include "wework_conversation.pb.h"
#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>

#else
typedef struct {
    int64_t val[2];
} int64x2_t;
#endif

namespace {
    constexpr size_t kPbBufferSize = 40;
    constexpr size_t kConvKeySize = 24;
    constexpr int kVidPrefixShift = 48;
    constexpr uint64_t kVidPrefix = 6;
    constexpr uint64_t kWeixinXidPrefix = 28;

    // 会话类型 convType 完整定义见 conv_type.h（与 libwework_framework 的 CONVTYPE 一致）

    // 内置服务会话的固定 conversation_id
    constexpr uint64_t kConvIdEmail = 10004;            // 邮件，convType=应用会话
    constexpr uint64_t kConvIdFileAssistant = 10006;    // 文件传输助手

    bool is_vid(uint64_t id) {
        return (id >> kVidPrefixShift) == kVidPrefix;
    }

    // 对应 libwework_framework 的 isWeixinXidUser：(id >> 48) == 28
    bool is_weixin_xid_user(uint64_t id) {
        return (id >> kVidPrefixShift) == kWeixinXidPrefix;
    }

    // 对应 isNormalUserId：vid 或微信 xid 均为单聊（个人会话）
    bool is_normal_user_id(uint64_t id) {
        return is_vid(id) || is_weixin_xid_user(id);
    }
    std::string build_conv_pb(ConvType conv_type,uint64_t local_id, uint64_t conversation_id) {
        sohook::ConversationInfo info;
        info.set_local_id(local_id);
        info.set_conversation_id(conversation_id);
        info.set_field_12(static_cast<uint32_t>(conv_type));
        info.set_field_15("");
        std::string out;
        (void)info.SerializeToString(&out);
        return out;
    }

    void release_conv_on_error(int64_t conv) {
        auto *ref = handle_ref_count(reinterpret_cast<void *>(conv));
        if (--(*ref) == 0) {
            handle_destroy(reinterpret_cast<void *>(conv));
        }
    }

    struct IndirectResult {
        uint64_t val1;
        uint64_t val2;
        uint64_t val3;
    };
}
ConvType resolve_conv_type(uint64_t conversation_id) {
    if (is_normal_user_id(conversation_id)) {
        return ConvType::kPerson;
    }
    if (conversation_id == kConvIdFileAssistant) {
        return ConvType::kFileAssistant;
    }
    if (conversation_id == kConvIdEmail) {
        return ConvType::kApp;
    }
    // 其余按群聊处理（与原 build_conv_pb 中非 vid 取 1 的行为一致）
    return ConvType::kRoom;
}
void *create_native_conversation(uint64_t remote_id) {
    using CreateConvObjFn = IndirectResult (*)();
    using InitPbFn = int64x2_t (*)(void *);
    auto create_cpp_empty_obj = reinterpret_cast<CreateConvObjFn>(
        get_abs_addr("libwework_framework.so", OFFSET_CREATE_CONV));
    auto init_buf = reinterpret_cast<InitPbFn>(
        get_abs_addr("libwework_framework.so", OFFSET_INIT_PB));
    auto parse_from_mem = reinterpret_cast<int64_t (*)(void *, void *)>(
        get_abs_addr("libwework_framework.so", OFFSET_PARSE_FROM_MEM));
    auto sync_to_conv = reinterpret_cast<void (*)(int64_t, void *)>(
        get_abs_addr("libwework_framework.so", OFFSET_SYNC_TO_CONV));
    auto destroy_buf = reinterpret_cast<void (*)(void *)>(
        get_abs_addr("libwework_framework.so", OFFSET_DESTROY_PB_STRUCT));
    if (!create_cpp_empty_obj || !init_buf || !parse_from_mem || !sync_to_conv ||
        !destroy_buf) {
        LOGE("create_conv_handle: func ptrs missing");
        return nullptr;
    }
    IndirectResult res = create_cpp_empty_obj();
    int64_t my_c_conv_ptr = res.val1;

    if (!my_c_conv_ptr) {
        LOGE("create_conv_handle: alloc failed");
        return nullptr;
    }

    uintptr_t internal_impl = reinterpret_cast<uintptr_t>(
        handle_impl(reinterpret_cast<void *>(my_c_conv_ptr)));
    if (!internal_impl) {
        LOGE("create_conv_handle: internal_impl is null");
        release_conv_on_error(my_c_conv_ptr);
        return nullptr;
    }

    uint64_t original_local_id = *reinterpret_cast<uint64_t *>(internal_impl +
                                                               OFFSET_CONV_LOCAL_ID);
    LOGD("create_conv_handle: original_local_id = %lu", original_local_id);
    alignas(16) uintptr_t pb_buffer[kPbBufferSize] = {0};
    init_buf(pb_buffer);
    ConvType conv_type = resolve_conv_type(remote_id);
    std::string conv_pb = build_conv_pb(conv_type,original_local_id,remote_id);
    // 【新增】：转换为 vector 并打印 Hex Dump
    std::vector<uint8_t> pb_vector(conv_pb.begin(), conv_pb.end());
    dump_protobuf_hex(pb_vector);
    if (!parse_from_mem(pb_buffer, &conv_pb)) {
        LOGE("create_conv_handle: parse pb failed");
        destroy_buf(pb_buffer);
        release_conv_on_error(my_c_conv_ptr);
        return nullptr;
    }

    sync_to_conv(internal_impl, pb_buffer);
    *reinterpret_cast<uint64_t *>(internal_impl + OFFSET_CONV_LOCAL_ID) = original_local_id;
    *reinterpret_cast<uint32_t *>(internal_impl + OFFSET_IMPL_HAS_BITS) |= FLAG_MSG_HAS_UNIQUE_ID;
    destroy_buf(pb_buffer);
    return (void *) my_c_conv_ptr;
}
