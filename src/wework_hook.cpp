//
// Created by user_wangzhen on 2026/5/11.
//
#include "dobby.h"
#include "../include/logger.h"
#include <jni.h>
#include <thread>
#include <chrono>
#include <vector>
#include "wework_hook.h"
#include <unistd.h>
#include "wework_message_factory.h"
#include "utils/address_utils.h"
#include "file_utils.h"
#include "wework_conversation_service.h"
#include "vtable_helper.h"
#include "main_thread_executor.h"
#include "offset.h"
#include "protocol_utils.h"
#include "conv_service.h"
#include "message/message_pb.h"
// 1.定义原函数指针，用户 Hook 之后调用原逻辑
// void (*orig_nativeSend)(JNIEnv* env, jobject thiz,jlong handle,jobject conv,jobject msg,jobject cb);
typedef void (*NativeSendFunc)(JNIEnv *env, jobject thiz, jlong handle, jobject conv, jobject msg, jobject cb);

NativeSendFunc orig_nativeSend = nullptr;
// 2.定义全局变量保存关键句柄
jlong g_wework_handle = 0;
// 增加全局变量保存 context 和 conv
jobject g_ctx = nullptr;
jobject g_conv = nullptr;
jobject g_thiz = nullptr;

void checkConvService(jlong handle) {
    uintptr_t my_conv_service = getConversationService();
    static auto unwrapNativeHandle = (uintptr_t (*)(uintptr_t)) get_absolute_address("libwework_framework.so", OFFSET_UNWRAP_NATIVE_HANDLE);
    if (unwrapNativeHandle) {
        uintptr_t v10 = unwrapNativeHandle((uintptr_t) handle);
        // v11 = (*v10 + 264)(v10)
        uintptr_t v11 = CallVirtualMethod(v10, 33); // 264/8
        // v12 = (*v11 + 40)(v11)
        uintptr_t intercept_service = CallVirtualMethod(v11, 5); // 40/8
        // 3. 打印对比结果
        LOGD(">>>> [对比测试] MyCreatedService: %p InterceptService: %p", (void*)my_conv_service,
             (void*)intercept_service);
        if (my_conv_service == intercept_service) {
            LOGI(">>>> [结论] 指针完全一致！getConversationService 逻辑正确。");
        } else {
            LOGE(">>>> [结论] 指针不一致！请检查偏移量或单例获取路径。");
        }
    }
}

// 3.拦截函数
void my_nativeSend(JNIEnv *env, jobject thiz, jlong handle, jobject conv, jobject msg, jobject cb) {
    LOGI(">>>> 拦截成功！当前消息 Handle: %lld <<<<", handle);
    // 2. 验证：如果 conv 和 msg 不为空，说明参数对齐了
    if (conv != nullptr && msg != nullptr) {
        LOGI(">>>> 参数对齐验证通过，准备执行原逻辑 <<<<");
        checkConvService(handle);

        // bool sendResult = hardcore_send_image_message();
        MessageParam text_task;
        text_task.msg_type = WeWorkMsgType::TEXT;
        text_task.text_content = "a message created by SoHook call native funcation";
        MessageParam img_task;
        img_task.msg_type = WeWorkMsgType::IMAGE;
        img_task.file_path = "/storage/emulated/0/Android/data/com.tencent.wework/files/tempimagecache/1688858339520293/de59a59e2f3ab19203f87f4ad65cf4c8_compress.png";

        MessageCallback my_perfect_listener;
        my_perfect_listener.onProgress = [](int64_t current, int64_t total, void* msg_handle) {
            double pct = total > 0 ? ((double)current / total) * 100.0 : 0.0;
            LOGI("[业务层高级扩展] 📈 正在上传，当前句柄: %p | 进度: %.2f%% (%ld/%ld)", msg_handle, pct, current, total);
        };
        my_perfect_listener.onResult = [](int code, void* conv_handle, void* msg_handle) {
            if (code == 0) {
                LOGI("[业务层高级扩展] 🎉 发送成功！会话指针: %p | 消息指针: %p", conv_handle, msg_handle);
                // 💡 可以在这里利用 conv_handle 或者是 msg_handle 传入其他 Hook 的 Native 函数进行联动
                // 1. 第一级寻址：解引用偏移 112 (0x70) 获取 internal_impl 指针
                auto* internal_impl_ptr = *reinterpret_cast<uintptr_t**>(
                    reinterpret_cast<char*>(conv_handle) + OFFSET_HANDLE_IMPL
                );
                if (!internal_impl_ptr) {
                    LOGE("[ExtractConvId] 错误: internal_impl 尚未初始化或为空");
                    return;
                }
                // 2. 第二级寻址：在 internal_impl 基础上偏移 200 (0xC8) 读取 8 字节的 uint64_t
                uint64_t conv_id = *reinterpret_cast<uint64_t*>(
                    reinterpret_cast<char*>(internal_impl_ptr) + OFFSET_CONVERSATION_ID
                );

                LOGI("[ExtractConvId] 成功从 conv_handle 逆向提取 ID: %llu", conv_id);
            } else {
                LOGE("[业务层高级扩展] ❌ 底层投递失败，错误码: %d", code);
            }
        };
        std::vector<uint64_t> id_list = {
            7881299599906412ULL,
            // 10758106104862420ULL,
            // 7881300507904689ULL,
            // 7881300527908908ULL,
            // 7881301482198287ULL
            // ... 后面有多少加多少
        };
        // 3. 遍历发射
        for (uint64_t cid : id_list) {
            // 发送文本消息
            send_model_message(cid,0, generate_text_message_pb("a message created by SoHook call native funcation"),my_perfect_listener);
            // 💡 逆向避坑小贴士：
            // 虽然我们做好了完美的引用计数管理，但在大批量（几十个甚至上百个群发）时，
            // 建议加上 50-100ms 的轻微延时，给企微底层的 TaskQueue 和网络线程让出缓冲时间。
            usleep(50000); // 50毫秒休眠

            // 发送图片消息
            uint64_t image_size = 0;
            uint32_t file_width = 0;
            uint32_t file_height = 0;
            const std::string path = "/storage/emulated/0/test.jpg";
            // 智能解析图片物理信息（仅在图片模式下跑，不污染文本模式）
            if (!get_image_info(path, image_size, file_width, file_height)) {
                LOGE("[-] 解析本地图片参数失败，路径: %s", path.c_str());
            }
            uint32_t thumb_w = file_width * 3 / 4;
            uint32_t thumb_h = file_height * 3 / 4;
            // send_model_message(cid,7, generate_image_message_pb(path,file_width,file_height,image_size,false,"","",file_width * 3 / 4,file_height * 3 / 4),my_perfect_listener);
            // 发送文件
            const std::string file_path = "content://com.wxsdk.app.share/test.pdf";

            // std::string sandbox_path = resolve_content_uri_to_local(env, file_path);
            // LOGI("[my_nativeSend] 企业微信沙盒路径: %s", sandbox_path.c_str());
            // uint64_t file_size = get_file_size(sandbox_path);
            // send_model_message(cid,8, generate_file_message_pb(sandbox_path,file_size),my_perfect_listener);

        }
        // int64_t sendResult =  send_model_message(img_task);
        // LOGI(">>>> [结论] 消息发送结果：%d",sendResult);
    }
    // 第一次拦截时，保存环境副本
    if (g_conv == nullptr) {
        g_conv = env->NewGlobalRef(conv);
        g_thiz = env->NewGlobalRef(thiz);
    }
    // 保存句柄供后续自动化使用
    g_wework_handle = handle;
    // 调用原函数，确保用户手动发送功能正常
    if (orig_nativeSend) {
        orig_nativeSend(env, thiz, handle, conv, msg, cb);
    }
}



// 4.初始化
void init_wework_hook() {
    std::thread([]() {
        LOGD("Hook Thread Started: Monitoring maps...");
        uintptr_t base_addr = 0;
        // 轮询等待，直到在 maps 中看到该库
        int loop_count = 0;
        while (true) {
            loop_count++;
            base_addr = get_module_base("libwework_framework.so");
            if (base_addr != 0) {
                break;
            }
            // 每 5 秒打一次日志，确认子线程依然活跃
            if (loop_count % 10 == 0) {
                LOGD("[HookThread] Still searching for libwework_framework.so...");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOGD("Found libwework_framework.so in maps at: %lx", base_addr);
        // 使用你 IDA 里的偏移量
        void *target_addr = (void *) (base_addr + OFFSET_NATIVE_MSG_SEND);
        LOGI("Final Hook Address: %p", target_addr);
        int ret = DobbyHook(target_addr, (dobby_dummy_func_t) my_nativeSend, (dobby_dummy_func_t *) &orig_nativeSend);
        if (ret == 0) {
            LOGI(">>>> Dobby Hook Success! <<<<");
        } else {
            LOGE("Dobby Hook Failed!");
        }
        // MainThreadExecutor::getInstance().post([]() {
        //     // 此处已经是纯正的 Android 主线程环境
        //     LOGI("Main Thread Executor Task Executed!");
        //
        // });
    }).detach();
}
