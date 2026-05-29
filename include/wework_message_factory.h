//
// Created by user_wangzhen on 2026/5/12.
//
#ifndef SOHOOK_WEWORK_MESSAGE_FACTORY_H
#define SOHOOK_WEWORK_MESSAGE_FACTORY_H

#include <jni.h>
#include <string>
#include <cstdint>
#include <functional>
// 💡 1. 放在头文件，方便所有引用了 factory 的 cpp 都能直接看到
enum class WeWorkMsgType {
    TEXT  = 1,
    IMAGE = 7
};

struct MessageParam {
    WeWorkMsgType msg_type;
    std::string text_content; // 文本消息使用
    std::string file_path;    // 图片/媒体消息使用
};
struct MessageCallback {
    // 进度回调：完全对齐 my_pure_native_onProgress 的参数
    std::function<void(int64_t current, int64_t total, void* msg_handle)> onProgress = nullptr;

    // 结果回调：完全对齐 my_pure_native_onResult 的参数
    std::function<void(int code, void* conv_handle, void* msg_handle)> onResult = nullptr;
};
// 建议统一使用 extern "C" 避免 C++ 符号名混淆
#ifdef __cplusplus
extern "C" {
#endif

jobject create_image_message(JNIEnv *env, const char *image_local_path);

jobject create_image_message_pure_native(JNIEnv *env, const char *path);

void *create_image_message_pure_native_ptr(
    const std::string &file_name,
    const std::string &file_path,
    uint64_t file_size,
    uint32_t width,
    uint32_t height);

void *create_text_message_pure_native_ptr(const std::string &text_content);
#ifdef __cplusplus
}
#endif

#endif
