//
// Created by user_wangzhen on 2026/5/19.
//
#include "message_sender.h"
#include <cstring>

#include "logger.h"
#include "utils/address_utils.h"
#include "message/msg_ptr.h"
#include "conv_service.h"
#include "offset.h"
#include "vtable_helper.h"


typedef int64_t (*AtomicDecRef)(unsigned int *result);
static AtomicDecRef native_dec_ref = nullptr;

typedef int64_t (*send_message)(int64_t, int64_t, int64_t *, int64_t, int64_t);

using ProgressCallback = std::function<void(int64_t, int64_t, uint64_t)>;
using ResultCallback = std::function<void(int, uint64_t)>;


// 从消息句柄的 impl 中提取 local_id
static uint64_t get_local_id_from_msg_handle(void *msg_handle) {
    if (!msg_handle) return 0;
    void *targetCore = handle_impl(msg_handle);
    if (!targetCore) return 0;
    return *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(targetCore) +
                                         OFFSET_MSG_LOCAL_ID);
}

// 从会话句柄逆向提取会话 ID（调试用）
static void extractConvId(void *conv_handle) {
    void *conv_impl_ptr = handle_impl(conv_handle);
    if (!conv_impl_ptr) {
        LOGE("[ExtractConvId] 错误: internal_impl 尚未初始化或为空");
        return;
    }
    uint64_t conv_id = *reinterpret_cast<uint64_t *>(
        reinterpret_cast<char *>(conv_impl_ptr) + OFFSET_CONVERSATION_ID);
    LOGD("[ExtractConvId] 成功从 conv_handle 逆向提取 ID: %llu", static_cast<unsigned long long>(conv_id));
}

// 释放 native 句柄的一个引用，计数归零时触发 deleting destructor
static void native_release_handle(void *handle) {
    if (!handle || !native_dec_ref) return;
    if ((native_dec_ref(handle_ref_count(handle)) & 1) != 0) {
        handle_destroy(handle);
    }
}

static int64_t my_custom_progress_invoker(uintptr_t *closure_ptr, uintptr_t /*cb_holder*/, uintptr_t /*msg_holder*/,
                                          uintptr_t /*conv_holder*/, int64_t current, int64_t total) {
    if (!closure_ptr) return 0;

    // 1. 从进度闭包取出消息句柄
    void *msg_handle = reinterpret_cast<void *>(closure_ptr[6]);
    // 2. 💡 顺藤摸瓜：通过索引 8 找到 Result 闭包，提取进度回调函数
    uintptr_t *fake_closure = reinterpret_cast<uintptr_t *>(closure_ptr[8]);
    if (fake_closure) {
        auto *progress_fn = reinterpret_cast<ProgressCallback *>(&fake_closure[10]);
        // 3. 💡 触发业务层自定义的进度监听，完整塞入 current, total, msg_handle
        if (progress_fn && *progress_fn) {
            (*progress_fn)(current, total, get_local_id_from_msg_handle(msg_handle));
        }
    }
    return 0;
}

static int64_t my_custom_result_invoker(uintptr_t *closure_ptr, uintptr_t /*cb_holder*/, uintptr_t /*msg_holder*/,
                                        uintptr_t /*conv_holder*/, uint32_t code) {
    if (!closure_ptr) return 0;

    void *msg_handle = reinterpret_cast<void *>(closure_ptr[8]);
    void *conv_handle = reinterpret_cast<void *>(closure_ptr[9]);
    extractConvId(conv_handle);

    // 1. 结果码由底层按值塞入 x4 寄存器，取出后驱动业务层回调
    const int real_code = static_cast<int>(code);
    auto *progress_fn = reinterpret_cast<ProgressCallback *>(&closure_ptr[10]);
    auto *result_fn = reinterpret_cast<ResultCallback *>(progress_fn + 1);
    // 💡 安全校验：确保 std::function 内部确实持有可调用实体
    if (result_fn && *result_fn) {
        (*result_fn)(real_code, get_local_id_from_msg_handle(msg_handle));
    }

    // 2. 💡 显式析构，清理两个 std::function 的内部捕获代理
    progress_fn->~ProgressCallback();
    result_fn->~ResultCallback();

    // 3. 释放消息/会话句柄引用（msg 对应 create 时手动持有的那一份）
    native_release_handle(msg_handle);
    native_release_handle(conv_handle);

    // 4. 释放闭包堆内存，完成全调用链生命周期闭环
    operator delete(closure_ptr);
    return 0;
}


int64_t send_model_message(uint64_t target_conv_id, int msg_type, std::vector<uint8_t> pb_data,
                           std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                           std::function<void(int, uint64_t)> onResult) {
    // 1. 解析 native 符号与全局服务
    uintptr_t so_base = get_module_base("libwework_framework.so");
    if (so_base == 0) {
        LOGE("so_base is null");
        return 0;
    }
    auto pfn_send_msg = reinterpret_cast<send_message>(so_base + OFFSET_MSG_SEND);
    if (!native_dec_ref) {
        native_dec_ref = reinterpret_cast<AtomicDecRef>(so_base + OFFSET_DEC_REF);
    }
    const uintptr_t g_conversation_service = getConversationService();
    if (!g_conversation_service) {
        LOGE("g_conversation_service is null");
        return 0;
    }

    // 2. 获取会话对象（走缓存查找）
    // void *conv_handle = create_native_conversation(target_conv_id);
    void *conv_handle = get_cache_conversation_by_key(0, target_conv_id);
    if (!conv_handle) {
        LOGE("get_cache_conversation_by_key failed");
        return 0;
    }

    // 💡 错误兜底：流程中途失败时释放句柄引用
    auto safety_cleanup = [conv_handle](void *msg_h) {
        native_release_handle(msg_h);
        native_release_handle(conv_handle);
    };

    // 3. 构造消息对象
    void *msg_handle = create_message_pure_native_ptr(msg_type, pb_data);
    if (!msg_handle) {
        LOGE("create_message_pure_native_ptr failed");
        safety_cleanup(nullptr);
        return 0;
    }
    int64_t msg_addr = reinterpret_cast<int64_t>(msg_handle);

    // 4. 构建 Result 闭包 (扩容至 0xC0 字节，自留地容纳 2 个 std::function)
    auto *result_closure = reinterpret_cast<uintptr_t *>(operator new(0xC0uLL));
    std::memset(result_closure, 0, 0xC0);

    *reinterpret_cast<uint32_t *>(result_closure) = 1; // ref_count
    result_closure[1] = reinterpret_cast<uintptr_t>(my_custom_result_invoker);
    result_closure[2] = 0LL;
    result_closure[3] = reinterpret_cast<uintptr_t>(so_base + OFFSET_MSG_SEND_CB);
    result_closure[5] = 0LL;
    result_closure[8] = reinterpret_cast<uintptr_t>(msg_handle);
    result_closure[9] = reinterpret_cast<uintptr_t>(conv_handle);
    // 💡 关键改造：从索引 10 开始，就地依次放置业务传入的两个回调函数
    auto *progress_space = reinterpret_cast<ProgressCallback *>(&result_closure[10]);
    new(progress_space) ProgressCallback(std::move(onProgress)); // placement new
    auto *result_space = reinterpret_cast<ResultCallback *>(progress_space + 1);
    new(result_space) ResultCallback(std::move(onResult)); // placement new

    uintptr_t result_std_fn_shell[2] = {reinterpret_cast<uintptr_t>(result_closure), 0};

    // 5. 构建 Progress 闭包 (索引 8 回挂 Result 闭包，共享业务回调)
    auto *progress_closure = reinterpret_cast<uintptr_t *>(operator new(0x50uLL));
    std::memset(progress_closure, 0, 0x50);
    *reinterpret_cast<uint32_t *>(progress_closure) = 1;
    progress_closure[1] = reinterpret_cast<uintptr_t>(my_custom_progress_invoker);
    progress_closure[6] = reinterpret_cast<uintptr_t>(msg_handle);
    progress_closure[7] = reinterpret_cast<uintptr_t>(conv_handle);
    // 💡 关键改造：把 Result 闭包的地址挂到 Progress 闭包的索引 8，方便顺藤摸瓜
    progress_closure[8] = reinterpret_cast<uintptr_t>(result_closure);

    uintptr_t progress_std_fn_shell[2] = {reinterpret_cast<uintptr_t>(progress_closure), 0};
    uint8_t conv_container[32] = {0};
    *reinterpret_cast<void **>(conv_container) = conv_handle;

    // 6. 调用底层发送虚函数
    CallVMethodByOffset<int64_t>(
        g_conversation_service, OFFSET_SEND_MSG_VIRT, // x0: Service
        reinterpret_cast<int64_t>(conv_container), // x1: Conversation 容器栈地址
        reinterpret_cast<int64_t>(&msg_addr), // x2: &msg_handle
        reinterpret_cast<int64_t>(progress_std_fn_shell), // x3: Progress std::function 栈壳地址
        reinterpret_cast<int64_t>(result_std_fn_shell) // x4: Result std::function 栈壳地址
    );
    // 备选：直接物理发射
    // int64_t result = pfn_send_msg(
    //     static_cast<int64_t>(g_conversation_service),
    //     reinterpret_cast<int64_t>(conv_handle),
    //     &msg_addr,
    //     reinterpret_cast<int64_t>(mock_progress_shell),
    //     reinterpret_cast<int64_t>(mock_callback_shell)
    // );
    return 1;
}
