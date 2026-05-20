//
// Created by user_wangzhen on 2026/4/15.
//
#pragma once
#include <android/log.h>
#define LOG_TAG "SoHook"

#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG,LOG_TAG, __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR,LOG_TAG, __VA_ARGS__))
#define LOGASSERT(...) ((void)__android_log_assert("0!=errno", LOG_TAG, __VA_ARGS__))