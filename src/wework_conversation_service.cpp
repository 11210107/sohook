//
// Created by user_wangzhen on 2026/5/19.
//
#include "wework_conversation_service.h"
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "file_utils.h"
#include "logger.h"
#include "address_utils.h"
#include "wework_conversation.h"
#include "wework_logic_center.h"
#include "wework_message_factory.h"

// ====================================================
// 1. 前置声明层（严格对齐签名）
// ====================================================
void my_pure_native_onResult(int code, void *conv_handle, void *msg_handle);

void my_pure_native_onProgress(int64_t current, int64_t total, void *msg_handle);

typedef void (*FnPureNativeOnResult)(int, void *, void *);

typedef void (*FnPureNativeOnProgress)(int64_t, int64_t, void *);

typedef int64_t (*AtomicDecRef)(unsigned int *result);

static AtomicDecRef native_dec_ref = nullptr;

int64_t my_custom_result_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4);

int64_t my_custom_progress_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4);

typedef int64_t (*send_message)(int64_t, int64_t, int64_t *, int64_t, int64_t);

typedef unsigned int *(*AtomicIncRef)(unsigned int *result);

// ====================================================
// 2. 主业务发射器：支持多类型动态分流
// ====================================================
int64_t send_model_message(uint64_t target_conv_id,const MessageParam& param) {
    LOGI("准备开始发送消息，类型ID: %d", static_cast<int>(param.msg_type));
    uintptr_t so_base = get_module_base("libwework_framework.so");
    if (so_base == 0) {
        LOGE("so_base is null");
        return 0;
    }

    auto pfn_send_msg = reinterpret_cast<send_message>(so_base + 0x25C9374);
    auto add_ref      = reinterpret_cast<AtomicIncRef>(so_base + 0x5EB5470);
    if (!native_dec_ref) {
        native_dec_ref = reinterpret_cast<AtomicDecRef>(so_base + 0x5EB5458);
    }

    uintptr_t g_conversation_service = getCurrentConvService();
    if (!g_conversation_service) {
        LOGE("g_conversation_service is null");
        return 0;
    }

    // 1. 创建会话对象
    void *conv_handle = create_and_inject_conversation(target_conv_id);
    if (!conv_handle) {
        LOGE("create_and_inject_conversation failed");
        return 0;
    }

    unsigned int *conv_ref = reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(conv_handle) + 96);
    if (conv_ref) {
        add_ref(conv_ref);
        LOGI("conv_ref current refcount: %u", *conv_ref);
    }

    // 💡 错误兜底 Lambda 闭包
    auto safety_cleanup = [conv_ref, conv_handle](void* msg_h) {
        if (msg_h && native_dec_ref) {
            unsigned int *m_ref = reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(msg_h) + 96);
            if ((native_dec_ref(m_ref) & 1) != 0) {
                (*reinterpret_cast<void (**)(void *)>(*reinterpret_cast<uintptr_t *>(msg_h) + 8LL))(msg_h);
            }
        }
        if (conv_handle && native_dec_ref && conv_ref) {
            if ((native_dec_ref(conv_ref) & 1) != 0) {
                (*reinterpret_cast<void (**)(void *)>(*reinterpret_cast<uintptr_t *>(conv_handle) + 8LL))(conv_handle);
            }
        }
    };

    // 2. 💡 核心改造：根据参数动态路由，构造不同的 Native 消息 Handle
    void *msg_handle = nullptr;

    if (param.msg_type == WeWorkMsgType::TEXT) {
        // ------ 文本分支 ------
        msg_handle = create_text_message_pure_native_ptr(param.text_content);
    }
    else if (param.msg_type == WeWorkMsgType::IMAGE) {
        // ------ 图片分支 ------
        uint64_t real_size = 0;
        uint32_t real_width = 0;
        uint32_t real_height = 0;

        // 智能解析图片物理信息（仅在图片模式下跑，不污染文本模式）
        if (!get_image_info(param.file_path, real_size, real_width, real_height)) {
            LOGE("[-] 解析本地图片参数失败，路径: %s", param.file_path.c_str());
            safety_cleanup(nullptr);
            return 0;
        }

        // 提取文件名
        size_t last_slash_idx = param.file_path.find_last_of("/");
        std::string file_name = (std::string::npos != last_slash_idx) ?
                                param.file_path.substr(last_slash_idx + 1) : param.file_path;

        msg_handle = create_image_message_pure_native_ptr(
            file_name,
            param.file_path,
            real_size,
            real_width,
            real_height
        );
    }

    // 统一检查消息对象是否伪造成功
    if (!msg_handle) {
        LOGE("create_message_pure_native_ptr failed");
        safety_cleanup(nullptr);
        return 0;
    }

    unsigned int *msg_ref = reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(msg_handle) + 96);
    if (msg_ref) {
        add_ref(msg_ref);
    }

    int64_t msg_addr = reinterpret_cast<int64_t>(msg_handle);

    // A. 构建 Progress 闭包空间
    auto *progress_closure = reinterpret_cast<uintptr_t *>(operator new(0x48uLL));
    std::memset(progress_closure, 0, 0x48);
    *reinterpret_cast<uint32_t *>(reinterpret_cast<char*>(progress_closure) + 0) = 1;
    progress_closure[1] = reinterpret_cast<uintptr_t>(my_custom_progress_invoker);
    progress_closure[4] = reinterpret_cast<uintptr_t>(my_pure_native_onProgress);
    progress_closure[6] = reinterpret_cast<uintptr_t>(msg_handle);
    progress_closure[7] = reinterpret_cast<uintptr_t>(conv_handle);

    uintptr_t mock_progress_shell[1] = {reinterpret_cast<uintptr_t>(progress_closure)};

    // B. 构建 Result 闭包空间
    auto *fake_closure = reinterpret_cast<uintptr_t *>(operator new(0x50uLL));
    std::memset(fake_closure, 0, 0x50);

    *reinterpret_cast<uint32_t *>(reinterpret_cast<char*>(fake_closure) + 0) = 1;
    fake_closure[1] = reinterpret_cast<uintptr_t>(my_custom_result_invoker);
    fake_closure[2] = 0LL;
    fake_closure[3] = reinterpret_cast<uintptr_t>(so_base + 0x5E9D49C);
    fake_closure[4] = reinterpret_cast<uintptr_t>(my_pure_native_onResult);
    fake_closure[5] = 0LL;
    fake_closure[8] = reinterpret_cast<uintptr_t>(msg_handle);
    fake_closure[9] = reinterpret_cast<uintptr_t>(conv_handle);

    uintptr_t mock_callback_shell[1] = {reinterpret_cast<uintptr_t>(fake_closure)};

    // C. 物理发射
    int64_t result = pfn_send_msg(
        static_cast<int64_t>(g_conversation_service),
        reinterpret_cast<int64_t>(conv_handle),
        &msg_addr,
        reinterpret_cast<int64_t>(mock_progress_shell),
        reinterpret_cast<int64_t>(mock_callback_shell)
    );

    LOGI("send_msg result %ld", result);
    return 1;
}

// ====================================================
// 3. 上层业务回调实现层（全局作用域平铺）
// ====================================================
void my_pure_native_onProgress(int64_t current, int64_t total, void *msg_handle) {
    double percent = total > 0 ? ((double) current / total) * 100.0 : 0.0;
    // 格式化修复：%lld 改为 %ld 消除 Clang 编译器警告
    LOGI("[⏳ Progress] 消息正在上传... 进度: %.2f%% (%ld / %ld) | Msg: %p",
         percent, current, total, msg_handle);
}

void my_pure_native_onResult(int code, void *conv_handle, void *msg_handle) {
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
        auto real_progress_target = (FnPureNativeOnProgress) closure_ptr[4];
        void *msg_handle = (void *) closure_ptr[6];

        int64_t current = (x2 != 0) ? *(int64_t *) x2 : 0;
        int64_t total = (x3 != 0) ? *(int64_t *) x3 : 0;

        if (real_progress_target) {
            real_progress_target(current, total, msg_handle);
        }
    }
    return 0;
}

int64_t my_custom_result_invoker(uintptr_t *closure_ptr, uintptr_t x1, uintptr_t x2, uintptr_t x3, uintptr_t x4) {
    LOGI("[SoHook_Bridge] 企微底层异步线程已唤醒我们的自定义跳板门！");
    if (closure_ptr) {
        auto real_target = (FnPureNativeOnResult) closure_ptr[4];
        void *msg_handle = (void *) closure_ptr[8];
        void *conv_handle = (void *) closure_ptr[9];

        int real_code = -1;
        if (x1 != 0) {
            real_code = *(int *) x1;
        }

        if (real_target) {
            real_target(real_code, conv_handle, msg_handle);
        }

        // 强引用 GC 释放
        if (msg_handle && native_dec_ref) {
            unsigned int *msg_ref = (unsigned int *) ((char *) msg_handle + 96);
            if ((native_dec_ref(msg_ref) & 1) != 0) {
                (*(void (**)(void *)) (*(uintptr_t *) msg_handle + 8LL))(msg_handle);
                LOGI("[SoHook_GC] 消息对象已完美析构释放");
            }
        }
        if (conv_handle && native_dec_ref) {
            unsigned int *conv_ref = (unsigned int *) ((char *) conv_handle + 96);
            if ((native_dec_ref(conv_ref) & 1) != 0) {
                (*(void (**)(void *)) (*(uintptr_t *) conv_handle + 8LL))(conv_handle);
                LOGI("[SoHook_GC] 会话对象已完美析构释放");
            }
        }

        operator delete(closure_ptr);
        LOGI("[SoHook_GC] 结果闭包堆内存已释放，全调用链生命周期闭环。");
    }
    return 0;
}
