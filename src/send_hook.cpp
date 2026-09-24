//
// Created by user_wangzhen on 2026/5/11.
//
#include "dobby.h"
#include "logger.h"
#include <jni.h>
#include <thread>
#include <chrono>
#include <vector>
#include "send_hook.h"
#include <unistd.h>
#include "utils/address_utils.h"
#include "utils/file_utils.h"
#include "message_sender.h"
#include "vtable_helper.h"
#include "main_thread_executor.h"
#include "offset.h"
#include "conv_service.h"
#include "message/message_pb.h"
#include "cdn_cache.h"
#include "image_broadcast.h"
// 1.定义原函数指针，用户 Hook 之后调用原逻辑
typedef void (*NativeSendFunc)(JNIEnv *env, jobject thiz, jlong handle, jobject conv, jobject msg, jobject cb);

NativeSendFunc orig_nativeSend = nullptr;
// 2.定义全局变量保存关键句柄
jlong g_wework_handle = 0;
// 保存 conv/thiz 全局引用
jobject g_conv = nullptr;
jobject g_thiz = nullptr;

void checkConvService(jlong handle) {
    uintptr_t my_conv_service = getConversationService();
    static auto unwrapNativeHandle = (uintptr_t (*)(uintptr_t)) get_absolute_address("libwework_framework.so", OFFSET_UNWRAP_NATIVE_HANDLE);
    if (unwrapNativeHandle) {
        uintptr_t v10 = unwrapNativeHandle((uintptr_t) handle);
        uintptr_t v11 = CallVMethodByOffset(v10, OFFSET_SERVICE_MANAGER);
        uintptr_t intercept_service = CallVMethodByOffset(v11, OFFSET_CONV_SERVICE);
        // 3. 打印对比结果
        LOGD("checkConvService [对比测试] MyCreatedService: %p InterceptService: %p", (void*)my_conv_service,
             (void*)intercept_service);
        if (my_conv_service == intercept_service) {
            LOGD("checkConvService [结论] 指针完全一致！getConversationService 逻辑正确。");
        } else {
            LOGE("checkConvService [结论] 指针不一致！请检查偏移量或单例获取路径。");
        }
    }
}

// 3.拦截函数
void my_nativeSend(JNIEnv *env, jobject thiz, jlong handle, jobject conv, jobject msg, jobject cb) {
    LOGD(">>>> 拦截成功！当前消息 Handle: %lld <<<<", static_cast<long long>(handle));
    // 2. 验证：如果 conv 和 msg 不为空，说明参数对齐了
    if (conv != nullptr && msg != nullptr) {
        checkConvService(handle);
        // bool sendResult = hardcore_send_image_message();
        auto on_progress = [](int64_t current, int64_t total, uint64_t msg_lid) {
            double pct = total > 0 ? ((double)current / total) * 100.0 : 0.0;
            LOGD("[on_progress] 📈 正在上传，当前消息 localId: %lu | 进度: %.2f%% (%ld/%ld)", msg_lid, pct, current, total);
        };
        auto on_result = [](int code, uint64_t msg_lid) {
            if (code == 0) {
                LOGD("[on_result] 🎉 发送成功！ | 消息 localId: %lu", msg_lid);
            } else {
                LOGE("[on_result] ❌ 底层投递失败，错误码: %d", code);
            }
        };
        /*
         ** 发送图片测试消息
        std::vector<uint64_t> id_list = {
            7881299599906412ULL,
            7881301969920617ULL,
            7881303349316267ULL,
            // ... 后面有多少加多少
        };
        // 方案1：同一张图 N 个联系人只上传一次 CDN——
        // 首个联系人正常发送(上传一次)，其余用捕获的 fileid/aes_key/md5 构建复用模型直发
        const std::string image_path = "/storage/emulated/0/test.jpg";
        send_image_to_contacts(image_path, id_list, on_progress, on_result);
        */
        /**
         *  发送测试消息*/
        std::vector<uint64_t> id_list = {
            7881299599906412ULL,
            // 7881301969920617ULL,
            // 7881303349316267ULL,
            // ... 后面有多少加多少
        };
        // 3. 遍历发射
        for (uint64_t cid : id_list) {
            // 发送文本消息
            // send_model_message(cid,MSG_TYPE_TEXT, generate_text_message_pb("a message created by SoHook call native funcation"),on_progress,on_result);
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
            send_model_message(cid,MSG_TYPE_IMAGE, generate_image_message_pb(path,file_width,file_height,image_size,false,"","",file_width * 3 / 4,file_height * 3 / 4),on_progress,on_result);
            // 发送文件
            const std::string file_path = "content://com.wxsdk.app.share/test.pdf";

            // std::string sandbox_path = resolve_content_uri_to_local(env, file_path);
            // LOGI("[my_nativeSend] 企业微信沙盒路径: %s", sandbox_path.c_str());
            // uint64_t file_size = get_file_size(sandbox_path);
            // send_model_message(cid,MSG_TYPE_FILE, generate_file_message_pb(sandbox_path,file_size),on_progress,on_result);

        }

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
void init_send_hook() {
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
        LOGD("Final Hook Address: %p", target_addr);
        int ret = DobbyHook(target_addr, (dobby_dummy_func_t) my_nativeSend, (dobby_dummy_func_t *) &orig_nativeSend);
        if (ret == 0) {
            LOGD(">>>> Dobby Hook Success! <<<<");
        } else {
            LOGE("Dobby Hook Failed!");
        }
        init_cdn_capture_hooks(base_addr); // CDN 结果捕获（方案1：fileid 免上传）
        // MainThreadExecutor::getInstance().post([]() {
        //     // 此处已经是纯正的 Android 主线程环境
        //     LOGI("Main Thread Executor Task Executed!");
        //
        // });
    }).detach();
}
