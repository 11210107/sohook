//
// Created by user_wangzhen on 2026/5/19.
//
#include "wework_conversation_service.h"

#include <cstring>
#include <cstdint>
#include <inttypes.h> // 提供 PRId64 支持

#include "logger.h"
#include "address_utils.h"
#include "wework_conversation.h"
#include "wework_logic_center.h"
#include "wework_message_factory.h"

// ====================================================
// 1. 前置声明层（严格对齐签名）
// ====================================================
void my_pure_native_onResult(int code, void* conv_handle, void* msg_handle);
void my_pure_native_onProgress(int64_t current, int64_t total, void* msg_handle);

typedef void (*FnPureNativeOnResult)(int, void*, void*);
typedef void (*FnPureNativeOnProgress)(int64_t, int64_t, void*);

typedef int64_t (*AtomicDecRef)(unsigned int *result);
static AtomicDecRef native_dec_ref = nullptr;

int64_t my_custom_result_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4);
int64_t my_custom_progress_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4);

typedef int64_t (*send_message)(int64_t, int64_t, int64_t *, int64_t, int64_t);
typedef unsigned int *(*AtomicIncRef)(unsigned int *result);

// ====================================================
// 2. 主业务发射器：send_model_message
// ====================================================
int64_t send_model_message() {
    LOGI("准备开始发送消息");
    uintptr_t so_base = get_module_base("libwework_framework.so");
    if (so_base == 0) {
        LOGE("so_base is null");
        return 0;
    }

    auto pfn_send_msg = (send_message) (so_base + 0x25C9374);
    auto add_ref = (AtomicIncRef) (so_base + 0x5EB5470);
    if (!native_dec_ref) {
        native_dec_ref = (AtomicDecRef)(so_base + 0x5EB5458);
    }

    uintptr_t g_conversation_service = getCurrentConvService();
    if (!g_conversation_service) {
        LOGE("g_conversation_service is null");
        return 0;
    }

    void *conv_handle = create_and_inject_conversation();
    if (!conv_handle) {
        LOGE("create_and_inject_conversation failed");
        return 0;
    }

    unsigned int *conv_ref = (unsigned int *) ((char *) conv_handle + 96);
    if (conv_ref) {
        add_ref(conv_ref);
        LOGI("conv_ref current refcount: %u", *conv_ref);
    }

    void *msg_handle = create_image_message_pure_native_ptr();
    if (!msg_handle) {
        LOGE("create_image_message_pure_native failed");
        return 0;
    }

    int64_t msg_addr = (int64_t) msg_handle;
    unsigned int *msg_ref = (unsigned int *) ((char *) msg_handle + 96);
    if (msg_ref) {
        add_ref(msg_ref);
    }

    // A. 构建 Progress 闭包空间
    auto* progress_closure = (uintptr_t*)operator new(0x48uLL);
    memset(progress_closure, 0, 0x48);
    *(uint32_t*)((char*)progress_closure + 0) = 1;
    progress_closure[1] = (uintptr_t)my_custom_progress_invoker;
    progress_closure[4] = (uintptr_t)my_pure_native_onProgress;
    progress_closure[6] = (uintptr_t)msg_handle;
    progress_closure[7] = (uintptr_t)conv_handle;

    uintptr_t mock_progress_shell[1] = { (uintptr_t)progress_closure };

    // B. 构建 Result 闭包空间
    auto* fake_closure = (uintptr_t*)operator new(0x50uLL);
    memset(fake_closure, 0, 0x50);

    *(uint32_t*)((char*)fake_closure + 0) = 1;
    fake_closure[1] = (uintptr_t)my_custom_result_invoker;
    fake_closure[2] = (uintptr_t)nullptr;
    fake_closure[3] = (uintptr_t)(so_base + 0x5E9D49C);

    fake_closure[4] = (uintptr_t)my_pure_native_onResult;
    fake_closure[5] = 0LL;

    fake_closure[8] = (uintptr_t)msg_handle;
    fake_closure[9] = (uintptr_t)conv_handle;

    uintptr_t mock_callback_shell[1] = { (uintptr_t)fake_closure };

    // 格式化修复：ARM64 下 int64_t 应当配合 %ld 或者 PRId64 打印
    int64_t result = pfn_send_msg(
        (int64_t) g_conversation_service,
        (int64_t) conv_handle,
        &msg_addr,
        (int64_t) mock_progress_shell,
        (int64_t) mock_callback_shell
    );

    LOGI("send_msg result %ld", result);
    return 1;
} // <--- 确保 send_model_message 函数到此闭合，绝对不向下嵌套！

// ====================================================
// 3. 上层业务回调实现层（全局作用域平铺）
// ====================================================
void my_pure_native_onProgress(int64_t current, int64_t total, void* msg_handle) {
    double percent = total > 0 ? ((double)current / total) * 100.0 : 0.0;
    // 格式化修复：%lld 改为 %ld 消除 Clang 编译器警告
    LOGI("[⏳ Progress] 消息正在上传... 进度: %.2f%% (%ld / %ld) | Msg: %p",
         percent, current, total, msg_handle);
}

void my_pure_native_onResult(int code, void* conv_handle, void* msg_handle) {
    LOGI("[🎉 Success] 纯 Native 消息发送流运转完毕！状态码: %d", code);
    if (code == 0) {
        LOGI(" -> 状态提示: 企微核心已成功接收该并投递！");
    } else {
        LOGE(" -> 错误提示: 底层投递失败，Code: %d", code);
    }
}

// ====================================================
// 4. 物理寄存器中转跳板门
// ====================================================
int64_t my_custom_progress_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4) {
    if (closure_ptr) {
        auto real_progress_target = (FnPureNativeOnProgress)closure_ptr[4];
        void* msg_handle = (void*)closure_ptr[6];

        int64_t current = (x2 != 0) ? *(int64_t*)x2 : 0;
        int64_t total   = (x3 != 0) ? *(int64_t*)x3 : 0;

        if (real_progress_target) {
            real_progress_target(current, total, msg_handle);
        }
    }
    return 0;
}

int64_t my_custom_result_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4) {
    LOGI("[SoHook_Bridge] 企微底层异步线程已唤醒我们的自定义跳板门！");
    if (closure_ptr) {
        auto real_target = (FnPureNativeOnResult)closure_ptr[4];
        void* msg_handle = (void*)closure_ptr[8];
        void* conv_handle = (void*)closure_ptr[9];

        int real_code = -1;
        if (x1 != 0) {
            real_code = *(int*)x1;
        }

        if (real_target) {
            real_target(real_code, conv_handle, msg_handle);
        }

        // 强引用 GC 释放
        if (msg_handle && native_dec_ref) {
            unsigned int *msg_ref = (unsigned int *) ((char *) msg_handle + 96);
            if ((native_dec_ref(msg_ref) & 1) != 0) {
                (*(void (**)(void*))(*(uintptr_t *)msg_handle + 8LL))(msg_handle);
                LOGI("[SoHook_GC] 消息对象已完美析构释放");
            }
        }
        if (conv_handle && native_dec_ref) {
            unsigned int *conv_ref = (unsigned int *) ((char *) conv_handle + 96);
            if ((native_dec_ref(conv_ref) & 1) != 0) {
                (*(void (**)(void*))(*(uintptr_t *)conv_handle + 8LL))(conv_handle);
                LOGI("[SoHook_GC] 会话对象已完美析构释放");
            }
        }

        operator delete(closure_ptr);
        LOGI("[SoHook_GC] 结果闭包堆内存已释放，全调用链生命周期闭环。");
    }
    return 0;
}