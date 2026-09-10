//
// Created by user_wangzhen on 2026/7/27.
//
#include "msg_ptr.h"
#include "logger.h"
#include "../offset.h"
#include "../utils/address_utils.h"

namespace {
    struct FakeStdString {
        size_t cap;
        size_t size;
        const unsigned char *data;
    };
}

void *create_message_pure_native_ptr(int type, std::vector<uint8_t> pb_data) {
    auto create_message_ptr = reinterpret_cast<uintptr_t (*)()>(get_absolute_address(
        "libwework_framework.so", OFFSET_CREATE_MSG_PTR));

    auto parse_pb_ptr = reinterpret_cast<uint64_t (*)(void *, void *)>(get_absolute_address(
        "libwework_framework.so", OFFSET_PARSE_FROM_MEM));

    if (!create_message_ptr) {
        LOGE("create_message: func ptrs missing");
        return nullptr;
    }

    void *msgHandle = (void *) create_message_ptr();
    if (!msgHandle) {
        LOGE("create_message_: alloc failed");
        return nullptr;
    }

    uint32_t *ref_count = (uint32_t *) ((char *) msgHandle + OFFSET_HANDLE_REF_COUNT);
    if (ref_count) (*ref_count)++;

    void *targetCore = *(void **) ((char *) msgHandle + OFFSET_HANDLE_IMPL);
    if (!targetCore) {
        LOGE("create_message_: core null");
        return nullptr;
    }
    uint64_t saved_local_id = *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(targetCore) +
                                                            OFFSET_MSG_LOCAL_ID);
    // *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(targetCore) + 24) = type;
    // 输出消息的 pb 数据
    //    dump_protobuf_hex(pb_data);

    if (pb_data.empty()) {
        LOGE("create_message_: pb empty");
        return nullptr;
    }

    FakeStdString fakeStr;
    fakeStr.cap = (pb_data.size() << 1) | 1;
    fakeStr.size = pb_data.size();
    fakeStr.data = pb_data.data();

    if (!parse_pb_ptr(targetCore, &fakeStr)) {
        LOGE("create_message_: parse pb failed");
        return nullptr;
    }
    *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(targetCore) + OFFSET_MSG_LOCAL_ID) = saved_local_id;
    *reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(targetCore) + OFFSET_IMPL_HAS_BITS) |= 8;
    LOGI("create_message_ ok: %p", msgHandle);
    return msgHandle;
}
