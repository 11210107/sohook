//
// Created by user_wangzhen on 2026/5/12.
//
#ifndef SOHOOK_WEWORK_MESSAGE_FACTORY_H
#define SOHOOK_WEWORK_MESSAGE_FACTORY_H

#include <jni.h>
#include <string>

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
#ifdef __cplusplus
}
#endif

#endif
