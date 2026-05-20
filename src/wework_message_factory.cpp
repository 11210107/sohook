//
// Created by user_wangzhen on 2026/5/12.
//
#include "wework_message_factory.h"

#include <cstdlib>
#include <cstring>

#include "logger.h"
#include "address_utils.h"
#include <stdint.h>
#include <dlfcn.h>
#include <string>
#include "protocol_utils.h"
#include "file_utils.h"

// 基础偏移定义 (基于你的反汇编结果)
#define OFFSET_HAS_BITS 16
#define OFFSET_MSG_TYPE 4
#define OFFSET_PATH_STRING 64
#define OFFSET_FILENAME_STRING 56
#define OFFSET_WIDTH 128
#define OFFSET_HEIGHT 136
// 引用计数偏移（根据 case 5u 推测）
#define OFFSET_REF_COUNT 96

/// --- 函数指针定义 ---
typedef void * (*Message_Constructor)(); // Handle 构造器 (0x137E7C4)
typedef void (*Core_Constructor)(void *core_ptr); // Core 构造器 (0x130530C)
typedef void * (*Get_Native_Handle)(JNIEnv *env, jobject obj); // 获取 Java 对象的 mNativeHandle
typedef bool (*ParseFromString_t)(void* core, void* std_str_ptr); // PB解析 (0x5F64AC4)
typedef void (*Core_CopyFrom)(void *dst, void *src); // 数据拷贝 (0x1307C5C)

// --- 静态变量 ---
static Message_Constructor create_message_ptr = nullptr;
// 生成 *msgHandle 从Java_com_tencent_wework_foundation_common_NativeHandleHolder_nativeNewObject方法里 case 5u:
static Core_Constructor init_core_ptr = nullptr;
static ParseFromString_t parse_pb_ptr = nullptr;
static Core_CopyFrom copy_core_ptr = nullptr;
// 初始化函数指针
void InitFunctions() {
    LOGI("[InitFunctions] 开始初始化函数指针...");
    // 1. 初始化构造器 (懒加载逻辑)
    LOGI("[InitFunctions] 开始解析 Native Message 构造器地址...");
    uintptr_t base = get_module_base("libwework_framework.so");
    if (base) {
        LOGI("[InitFunctions] libwework_framework.so 基地址: %p", base);
        create_message_ptr = (Message_Constructor)(base + 0x137E7C4);
        init_core_ptr      = (Core_Constructor)(base + 0x130530C);
        parse_pb_ptr       = (ParseFromString_t)(base + 0x5F64AC4);
        copy_core_ptr      = (Core_CopyFrom)(base + 0x1307C5C);
    } else {
        LOGE("[InitFunctions] 获取 Native Message 构造器地址失败! libwework_framework.so 可能未加载");
    }
}

jobject create_image_message(JNIEnv *env, const char *image_local_path) {
    LOGI("======= 开始构建伪造图片消息 =======");
    LOGI("[+] 目标路径: %s", image_local_path);
    jstring jPath = env->NewStringUTF(image_local_path);

    // 1.获取图片尺寸
    jclass ImageUtilsClass = env->FindClass("hyl");
    if (!ImageUtilsClass) {
        LOGE("[-] 找不到 hyl 类");
        return nullptr;
    }
    jmethodID getBitmapSize = env->GetStaticMethodID(ImageUtilsClass, "z",
                                                     "(Ljava/lang/String;)Landroid/graphics/Point;");
    jobject point = env->CallStaticObjectMethod(ImageUtilsClass, getBitmapSize, jPath);
    if (!point) {
        LOGE("[-] 获取 Point 失败，请检查路径权限");
        return nullptr;
    }

    jclass PointClass = env->FindClass("android/graphics/Point");
    int width = env->GetIntField(point, env->GetFieldID(PointClass, "x", "I"));
    int height = env->GetIntField(point, env->GetFieldID(PointClass, "y", "I"));
    LOGI("[+] 解析宽高成功: %d x %d", width, height);
    // 2.利用MessageManager（hss）的 H1构造 FileMessage(PB对象)
    jclass MessageManagerClass = env->FindClass("hss");
    // 注意：根据你的源码，H1 的签名比较长，这里用真实签名替换 ...
    jmethodID h1Method = env->GetStaticMethodID(MessageManagerClass, "H1",
                                                "(Ljava/lang/String;IIIZLkotlin/jvm/functions/Function2;)Lcom/tencent/wework/foundation/model/pb/WwRichmessage$FileMessage;");
    if (!h1Method) {
        LOGE("[-] 找不到 hss.H1 方法，请检查签名");
        return nullptr;
    }
    jobject fileMessage = env->CallStaticObjectMethod(MessageManagerClass, h1Method, jPath, width, height, 0, false,
                                                      nullptr);
    if (!fileMessage) {
        LOGE("[-] hss.H1 执行返回为空");
        return nullptr;
    }
    LOGI("[+] FileMessage PB 对象构造完成");
    // 3.构建 WwMessage.Message (Java Bean)
    jclass wwMsgClass = env->FindClass("com/tencent/wework/foundation/model/pb/WwMessage$Message");
    jobject wwMsgObj = env->NewObject(wwMsgClass, env->GetMethodID(wwMsgClass, "<init>", "()V"));

    // 序列化 FileMessage
    jclass nanoClass = env->FindClass(
        "com/google/protobuf/nano/MessageNano"
    );
    if (!nanoClass) {
        LOGE("nanoClass null");
        return nullptr;
    }
    jmethodID toByteMethod = env->GetStaticMethodID(
        nanoClass,
        "toByteArray",
        "(Lcom/google/protobuf/nano/MessageNano;)[B"
    );
    if (!toByteMethod) {
        LOGE("toByteArray method null");

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }

        return nullptr;
    }
    jbyteArray contentBytes =
            (jbyteArray) env->CallStaticObjectMethod(
                nanoClass,
                toByteMethod,
                fileMessage
            );
    if (contentBytes) {
        env->SetObjectField(wwMsgObj, env->GetFieldID(wwMsgClass, "content", "[B"), contentBytes);
        env->SetIntField(wwMsgObj, env->GetFieldID(wwMsgClass, "contentType", "I"), 7);
        LOGI("[+] WwMessage.Message 容器序列化填充成功");
    } else {
        LOGE("[-] 序列化 FileMessage 失败");
        return nullptr;
    }
    // 4.生成带 Native Handle 的核心 Message 对象
    jclass MsgClass = env->FindClass("com/tencent/wework/foundation/model/Message");
    //com.tencent.wework.foundation.model.Message
    jmethodID newMessageMethod = env->GetStaticMethodID(MsgClass, "NewMessage",
                                                        "()Lcom/tencent/wework/foundation/model/Message;");
    jobject finalMessage = env->CallStaticObjectMethod(MsgClass, newMessageMethod);
    if (!finalMessage) {
        LOGE("[-] Message.NewMessage() 失败，可能 Native 层内存不足");
        return nullptr;
    }
    // 5.绑定数据
    jmethodID setInfoMethod = env->GetMethodID(MsgClass, "setInfo",
                                               "(Lcom/tencent/wework/foundation/model/pb/WwMessage$Message;)V");
    env->CallVoidMethod(finalMessage, setInfoMethod, wwMsgObj);
    LOGI("[+] ======= 伪造图片消息构建成功! =======");
    return finalMessage;
}


// --- 第二步：给 Java 层使用的接口 ---
jobject create_image_message_pure_native(JNIEnv *env, const char *path) {
    LOGI("======= 纯 Native 协议级构建消息开始 =======");
    // 步骤 0: 初始化函数指针
    InitFunctions();
    if (!create_message_ptr) {
        LOGE("[create_image_message_pure_native] 关键函数指针缺失，初始化失败 create_message_ptr: %p",
             create_message_ptr);
        return nullptr;
    }
    // 1. 创建官方 Handle 对象 (0x78字节)
    void* msgHandle = (void*)create_message_ptr();
    if (!msgHandle) {
        LOGE("[create_image_message_pure_native] Native 构造器返回空指针!");
        return nullptr;
    }
    // 2. 增加引用计数 (Offset 96 in Handle)
    // 必须做，否则对象可能被底层清理任务回收
    uint32_t *ref_count = (uint32_t *) ((char *) msgHandle + 96);
    if (!ref_count) {
        LOGE("[create_image_message_pure_native] ref_count 引用计数返回空指针!");
        return nullptr;
    }
    (*ref_count)++;

    // 3. 获取核心 Core 指针
    void* targetCore = *(void**)((char*)msgHandle + 112);
    if (!targetCore) {
        LOGE("[create_image_message_pure_native] 核心 Core 指针返回空指针!");
        return nullptr;
    }
    // 强制修正为图片类型 (2)
    *(int32_t*)((char*)targetCore + 24) = 7;
    // 4. 在栈上构造临时的 PB 实体 (模拟 nativeSetInfo 局部变量 v19)
    char tempCore[264];
    init_core_ptr(tempCore); // 调用 sub_130530C 初始化虚表和成员


    // 5. 将图片 PB 字节流解析到 tempCore 中
    // 注意：sub_5F64AC4 期待的是一个 std::string 指针或特定的 Buffer 结构
    // 这里我们简单模拟 nativeSetInfo 中 sub_1216860 转换后的结果
    // 如果无法直接调用 parse_pb_ptr，可尝试寻找直接解析 buffer 的函数
    std::string bin_path = "/storage/emulated/0/Android/data/com.tencent.wework/files/image_message.bin";
    std::vector<uint8_t> pb_data = read_binary_file(bin_path);
    // size_t pb_len = sizeof(raw_pb);
    struct FakeStdString {
        size_t cap;
        size_t size;
        const unsigned char* data;
    };
    FakeStdString fakeStr;
    fakeStr.cap = (pb_data.size() << 1) | 1; // 标记为长字符串模式
    fakeStr.size = pb_data.size();
    fakeStr.data = pb_data.data();
    // 调用解析函数
    bool success = parse_pb_ptr(targetCore, &fakeStr);
    if (success) {
        LOGI("Protobuf 解析成功！数据已装载至 Core");
    } else {
        LOGE("Protobuf 解析失败，请检查 PB 格式或 Tag");
        return nullptr;
    }
    // 【关键替换点】：如果你已经有解析好的 tempCore 或者通过其他方式填充了数据
    // 我们直接将数据拷贝到 targetCore
    // 假设此处你已经通过某种方式让 tempCore 拥有了图片 PB 数据
    // copy_core_ptr(targetCore, tempCore);

    // 6. 模拟 sub_12181CC 的二级指针包装逻辑
    // 企微 Java 层 Message.mNativeHandle = 指向 msgHandle 地址的指针
    void** wrapper_ptr = (void**)operator new(8);
    *wrapper_ptr = msgHandle;
    if (!wrapper_ptr) {
        LOGE("[create_image_message_pure_native] wrapper_ptr返回空指针!");
        return nullptr;
    }
    *wrapper_ptr = msgHandle;

    // 7. 实例化 Java Message 对象
    // 建议：直接找构造函数 <init>(J)V
    jclass MsgClass = env->FindClass("com/tencent/wework/foundation/model/Message");
    jmethodID initMethod = env->GetMethodID(MsgClass, "<init>", "(J)V");

    // 将二级指针传给构造函数
    jobject finalMsg = env->NewObject(MsgClass, initMethod, (jlong) wrapper_ptr);

    LOGI("[+] ======= 纯 Native 伪造图片消息构建成功! ======= ");
    return finalMsg;
}


// 修改返回值类型为 void*，去掉了 JNIEnv 参数，使其成为纯 Native 辅助函数
void* create_image_message_pure_native_ptr(
    const std::string& file_name,
    const std::string& file_path,
    uint64_t file_size,
    uint32_t width,
    uint32_t height
) {
    LOGI("======= 纯 Native 协议级构建消息开始 =======");

    // 步骤 0: 初始化函数指针
    InitFunctions();
    if (!create_message_ptr) {
        LOGE("[create_image_message_pure_native] 关键函数指针缺失！");
        return nullptr;
    }

    // 1. 创建官方 Handle 对象 (0x78字节)
    void* msgHandle = (void*)create_message_ptr();
    if (!msgHandle) {
        LOGE("[create_image_message_pure_native] Native 构造器返回空指针!");
        return nullptr;
    }

    // 2. 增加引用计数 (Offset 96 in Handle)
    // 既然要传给底层发消息引擎，引擎发送完后通常会调用 sub_5EB5458 进行 Release (Ref-1)
    // 如果不在这里 AddRef，发送完瞬间这个对象就会被底层析构，导致闪退或野指针
    uint32_t *ref_count = (uint32_t *) ((char *) msgHandle + 96);
    if (ref_count) {
        (*ref_count)++;
        LOGI("[create_image_message_pure_native] AddRef 成功，当前引用计数: %u", *ref_count);
    }

    // 3. 获取核心 Core 指针
    void* targetCore = *(void**)((char*)msgHandle + 112);
    if (!targetCore) {
        LOGE("[create_image_message_pure_native] 核心 Core 指针返回空指针!");
        // 记得异常时释放内存
        return nullptr;
    }

    // 强制修正为图片类型 (企业微信图片类型通常是 7，保持你的设定)
    *(int32_t*)((char*)targetCore + 24) = 7;

    // 5. 将图片 PB 字节流解析到 targetCore 中
    // 💡 注意：你需要在这里根据传入的 path 动态生成图片的 raw_pb（包含图片长宽、MD5、大小、CDN Url 等）
    // 这里暂时沿用你的静态 fakeStr 逻辑
    uint32_t thumb_w = 580;
    uint32_t thumb_h = 558;
    std::vector<uint8_t> pb_data = generate_image_message_proto(
        file_name,
        file_path,
        file_size,
        width, height,
        thumb_w, thumb_h,
        "/storage/emulated/0/Android/data/com.tencent.wework/files/uploadTempThumbimage/8ece24478cd7f75a2aaf033b56b129c5.thumbimage",
        "/storage/emulated/0/Android/data/com.tencent.wework/files/uploadTempMidbimage/8ece24478cd7f75a2aaf033b56b129c5.midimage"
    );
    // 1. 💡 从本地路径动态读取二进制 PB 文件
    // std::string bin_path = "/storage/emulated/0/Android/data/com.tencent.wework/files/image_message.bin";
    // std::vector<uint8_t> pb_data = read_binary_file(bin_path);
    // 2. 💡 核心：在这里把动态生成的内存内容彻底暴露在日志里！
    dump_protobuf_hex(pb_data);

    if (pb_data.empty()) {
        LOGE("[-] 读取本地 PB 文件为空，构建中断");
        return nullptr;
    }
    // size_t pb_len = sizeof(raw_pb);
    struct FakeStdString {
        size_t cap;
        size_t size;
        const unsigned char* data;
    };
    FakeStdString fakeStr;
    fakeStr.cap = (pb_data.size() << 1) | 1;
    fakeStr.size = pb_data.size();
    fakeStr.data = pb_data.data();
    // fakeStr.data = reinterpret_cast<const unsigned char*>(pb_data.data());

    if (parse_pb_ptr) {
        bool success = parse_pb_ptr(targetCore, &fakeStr);
        if (success) {
            LOGI("Protobuf 解析成功！图片数据已装载至 Core");
        } else {
            LOGE("Protobuf 解析失败，请检查 PB 格式或 Tag");
            return nullptr;
        }
    }

    LOGI("[+] ======= 纯 Native 伪造图片对象构建成功! 指针: %p ======= ", msgHandle);

    // 直接返回官方的一级 Handle 指针
    return msgHandle;
}