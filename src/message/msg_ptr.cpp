//
// Created by user_wangzhen on 2026/7/27.
//
#include "msg_ptr.h"
#include "logger.h"
#include "../offset.h"
#include "address_utils.h"

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
    auto init_core_ptr = reinterpret_cast<uintptr_t (*)(uintptr_t)>(get_absolute_address(
        "libwework_framework.so", OFFSET_INIT_CORE_PTR));
    auto parse_pb_ptr = reinterpret_cast<uint64_t (*)(void *, void *)>(get_absolute_address(
        "libwework_framework.so", OFFSET_PARSE_FROM_MEM));
    auto copy_core_ptr = reinterpret_cast<uint64_t (*)(void *, void *)>(get_absolute_address(
        "libwework_framework.so", OFFSET_COPY_CORE_PTR));
    if (!create_message_ptr) {
        LOGE("create_image_msg: func ptrs missing");
        return nullptr;
    }

    void *msgHandle = (void *) create_message_ptr();
    if (!msgHandle) {
        LOGE("create_image_msg: alloc failed");
        return nullptr;
    }

    uint32_t *ref_count = (uint32_t *) ((char *) msgHandle + 96);
    if (ref_count) (*ref_count)++;

    void *targetCore = *(void **) ((char *) msgHandle + 112);
    if (!targetCore) {
        LOGE("create_image_msg: core null");
        return nullptr;
    }
    uint64_t saved_local_id = *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(targetCore) +
                                                            128);
    // *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(targetCore) + 24) = type;
    // 输出消息的 pb 数据
    //    dump_protobuf_hex(pb_data);

    if (pb_data.empty()) {
        LOGE("create_image_msg: pb empty");
        return nullptr;
    }

    FakeStdString fakeStr;
    fakeStr.cap = (pb_data.size() << 1) | 1;
    fakeStr.size = pb_data.size();
    fakeStr.data = pb_data.data();

    if (!parse_pb_ptr(targetCore, &fakeStr)) {
        LOGE("create_image_msg: parse pb failed");
        return nullptr;
    }
    *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(targetCore) + 128) = saved_local_id;
    *reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(targetCore) + 16) |= 0x200;
    LOGI("create_image_msg ok: %p", msgHandle);
    return msgHandle;
}
