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
#include "address_utils.h"
#include "wework_logic_center.h"
#include "wework_conversation_service.h"
#include "vtable_helper.h"
#include "wework_conversation.h"
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
// 3.拦截函数
void my_nativeSend(JNIEnv *env, jobject thiz, jlong handle, jobject conv, jobject msg, jobject cb) {
    LOGI(">>>> 拦截成功！当前消息 Handle: %lld <<<<", handle);
    // 2. 验证：如果 conv 和 msg 不为空，说明参数对齐了
    if (conv != nullptr && msg != nullptr) {
        LOGI(">>>> 参数对齐验证通过，准备执行原逻辑 <<<<");
        uintptr_t my_conv_service = getCurrentConvService();
        static auto sub_1217284 = (uintptr_t (*)(uintptr_t)) get_absolute_address("libwework_framework.so", 0x1217284);
        if (sub_1217284) {
            uintptr_t v10 = sub_1217284((uintptr_t) handle);
            // v11 = (*v10 + 264)(v10)
            uintptr_t v11 = CallVirtualMethod(v10, 33); // 264/8
            // v12 = (*v11 + 104)(v11)
            uintptr_t intercept_service = CallVirtualMethod(v11, 13); // 104/8
            // 3. 打印对比结果
            LOGI(">>>> [对比测试] MyCreatedService: %p InterceptService: %p", (void*)my_conv_service,
                 (void*)intercept_service);
            if (my_conv_service == intercept_service) {
                LOGI(">>>> [结论] 指针完全一致！getCurrentConvService 逻辑正确。");
            } else {
                LOGE(">>>> [结论] 指针不一致！请检查偏移量或单例获取路径。");
            }
        }

        // uintptr_t my_create_conv = create_and_inject_conversation();
        // bool sendResult = hardcore_send_image_message();
        MessageParam text_task;
        text_task.msg_type = WeWorkMsgType::TEXT;
        text_task.text_content = "a message created by SoHook call native funcation";
        MessageParam img_task;
        img_task.msg_type = WeWorkMsgType::IMAGE;
        img_task.file_path = "/storage/emulated/0/Android/data/com.tencent.wework/files/tempimagecache/1688858339520293/de59a59e2f3ab19203f87f4ad65cf4c8_compress.png";
        std::vector<uint64_t> id_list = {
            7881299599906412ULL,
            7881300200069932ULL,
            7881300507904689ULL,
            7881300527908908ULL,
            7881301482198287ULL
            // ... 后面有多少加多少
        };
        // 3. 遍历发射
        for (uint64_t cid : id_list) {
            send_model_message(cid, text_task);
            // 💡 逆向避坑小贴士：
            // 虽然我们做好了完美的引用计数管理，但在大批量（几十个甚至上百个群发）时，
            // 建议加上 50-100ms 的轻微延时，给企微底层的 TaskQueue 和网络线程让出缓冲时间。
            usleep(50000); // 50毫秒休眠
        }
        // int64_t sendResult =  send_model_message(img_task);
        // LOGI(">>>> [结论] 消息发送结果：%d",sendResult);
    }
    // 第一次拦截时，保存环境副本
    if (g_conv == nullptr) {
        g_conv = env->NewGlobalRef(conv);
        g_thiz = env->NewGlobalRef(thiz);
    }
    const char *test_path = "/storage/emulated/0/Android/data/com.tencent.wework/files/20260414104931_149_83.jpg";
    // jobject fake_msg = create_image_message(env, test_path);
    jobject fake_msg = create_image_message_pure_native(env, test_path);
    if (fake_msg && orig_nativeSend) {
        LOGI(">>>> 触发自动重发测试... <<<<");

        // orig_nativeSend(env, g_thiz, handle, g_conv, fake_msg, nullptr);
    }
    // 保存句柄供后续自动化使用
    g_wework_handle = handle;
    // 调用原函数，确保用户手动发送功能正常
    // 调用原函数，保证业务不中断
    if (orig_nativeSend) {
        orig_nativeSend(env, thiz, handle, conv, msg, cb);
    }
}

// 辅助函数：从 /proc/self/maps 中读取基地址
uintptr_t get_module_base_addr(const char *module_name) {
    uintptr_t addr = 0;
    char line[1024];
    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, module_name)) {
                addr = strtoull(line, NULL, 16);
                break;
            }
        }
        fclose(fp);
    }
    return addr;
}

// 4.初始化
void init_wework_hook() {
    std::thread([]() {
        LOGI("Hook Thread Started: Monitoring maps...");
        uintptr_t base_addr = 0;

        // 轮询等待，直到在 maps 中看到该库
        while (true) {
            base_addr = get_module_base_addr("libwework_framework.so");
            if (base_addr != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOGI("Found libwework_framework.so in maps at: %lx", base_addr);

        // 使用你 IDA 里的偏移量
        void *target_addr = (void *) (base_addr + 0xFFACC8);
        LOGI("Final Hook Address: %p", target_addr);

        int ret = DobbyHook(target_addr, (dobby_dummy_func_t) my_nativeSend, (dobby_dummy_func_t *) &orig_nativeSend);
        if (ret == 0) {
            LOGI(">>>> Dobby Hook Success! <<<<");
        } else {
            LOGE("Dobby Hook Failed!");
        }
    }).detach();
}
