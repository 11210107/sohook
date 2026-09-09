// zygisk inject - native_msg 本地分支专用
#include <string.h>
#include <dlfcn.h>
#include <cstring>
#include "../include/logger.h"
#include "dobby.h"
#include "utils/address_utils.h"

static void *g_orig_dlopen_ext = nullptr;
static bool g_hooks_installed = false;

static void install_all_hooks() {
    if (g_hooks_installed) return;
    g_hooks_installed = true;
    LOGD("install_all_hooks: libwechatnormsg_stl.so detected, installing hooks");
}

static void *my_android_dlopen_ext(const char *filename, int flags,
                                   const void *extinfo) {
    auto orig = reinterpret_cast<void *(*)(const char *, int,
                                           const void *)>(
            g_orig_dlopen_ext);
    void *result = orig(filename, flags, extinfo);

    if (!g_hooks_installed && filename &&
        strstr(filename, "libwechatnormsg_stl.so")) {
        install_all_hooks();
    }

    return result;
}


__attribute__((constructor))
void init_hook_dlopen() {
    // 1. 拦截 android_dlopen_ext，确保在目标 so 加载的瞬间安装 hook
    void *dlopen_ext = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    if (dlopen_ext) {
        DobbyHook(dlopen_ext,
                        reinterpret_cast<void *>(my_android_dlopen_ext),
                        &g_orig_dlopen_ext);
        if (g_orig_dlopen_ext) {
            LOGD("init_hook_dlopen: android_dlopen_ext hooked");
        } else {
            LOGE("init_hook_dlopen: Dobby failed");
        }
    } else {
        LOGE("init_hook_dlopen: android_dlopen_ext not found");
    }

    // 2. 兜底：如果目标 so 已经加载（极少情况）
    if (!g_hooks_installed && is_so_loaded("libwechatnormsg_stl.so")) {
        LOGD("init_hook_dlopen: so already loaded, installing hooks");
        install_all_hooks();
    }
}
