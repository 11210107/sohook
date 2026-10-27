//
// cdn_url_exchange.cpp — 独立 CDN 上传 + 服务端换 URL（复刻自定义表情 ylj.d0 链路）
//
// 机制证据链（全部经 IDA 反编译 + trace/addCustomEmojiInfo.md 实测闭环）：
//  * 上传半程：Java FileService.CdnUploadFileOnPath(path,1,cb,prog,42)
//    → JNI 0x11EA680：unwrap→vt+264 取 SM→vt+48 取 FileService→vt+192(请求*,空ref,空ref)。
//    a6/a7 回调对象为 null 时 result/progress 两个 ClosureRef 保持 0（IsSameObject
//    分支实证，App 原生支持空回调）→ 我们传两个零值 ref 即与 App 空回调分支同构。
//    服务端（"/WeWorkService.WeWorkRpcService/CdnUploadFileOnPath" RPC）最终经
//    sub_50C6934 创建 CdnUploadTask（sub_2B87CBC）→ OnUploadFinish(0x2B88B70)
//    汇聚点必然回调 → 既有 cdn_cache 观测 hook 按 path 捕获三元组，零伪造。
//  * 请求结构（ctor 0x15CF4C8/dtor 0x15CF6A0 实测）：64B —— vt@+0(off_A537C48)、
//    has_bits@+16（|=1 path 在场、|=0x38 三 int 在场）、三个 pbc string 槽
//    @+24/+32/+40（未置位=哨兵值 so_base+0xAB487F8，本 JNI 只置 +24 路径）、
//    scene@+48/subscene@+52/type@+56（App 表情流值 1/42/-1）。
//    字段置位/赋值直接调用框架自有 sub_FB841C(slot, const std::string*)
//    （new 0x18 拷贝构造）/ sub_F9C97C(str*, data, len)（已置位兜底），
//    不自行实现 pbc 内部——与 JNI 逐行同构。
//  * 兑换半程：JNI 0x10B6838：unwrap→vt+264(SM)→vt+376(Collection 服务)→vt+112
//    (CollectionBackend)→vt+440(fileId&, aeskey&, md5&, ClosureRef&)。
//    完成 ABI（backend 0x50C35D8 → OnGetGifUrlCallback 0x50C3C28 → deferred
//    0x78 闭包 invoke 0x2860C54 实证）：
//      my_invoke(closure, int* errcode, std::string* s1, s2, s3)
//    —— errcode 按指针；三串归 deferred 闭包所有（@+48/+72/+96），只读勿释放；
//    我的 ref 流转：backend 闭包+56(AddRef)→task+480→deferred+32(AddRef)→
//    deferred 释放(sub_2860D38→sub_6D36030)时归零→destroy 收尾。闭包构造走框架
//    自有 ctor sub_6D35F44（+0 计数=1/+8 invoke/+16 destroy/+24=noop 0x6D35F60），
//    +32 起为自有捕获区（框架对闭包仅触碰 +0/+8/+16/+24，message_sender 先例）。
//  * 回调模式（可选，url.req 注释行 mode=cb）：复刻 JNI 非空回调分支（L43-55/L61-75）
//    —— new 0x40 → sub_6D35F44(closure, invoke, destroy) → +32 起捕获区 → 槽位=裸
//    闭包指针（sub_6D3603C 纯赋值实证无 addref）→ vt+192 调毕 sub_6D36044 释放自有
//    引用，框架侧同步 addref 持有至任务完成。result invoke ABI（sub_11EF96C 实证）：
//    (closure, bool* ok, str* fileid, str* aeskey, str* md5)——ok 按指针，三串归上传
//    任务所有只读；progress invoke ABI（sub_11EFB7C 实证）：(closure, str* ident,
//    int64* cur, int64* total)——两计数器按指针。完成回调直接投递主线程兑换任务
//    ——无共享状态、无轮询、不经 [cdn_cache] 取结果（无缓存快路/回填）。
//    全量变体（upload_and_exchange_gif_url_full，图片→表情链路）：兑换回调外再包
//    一层——到达时补齐上传三元组，七元组一次交付调用方。
//
// 线程模型：原生调用一律经 MainThreadExecutor 投递主线程（排在 orig_nativeSend
// 之后，避免发送管线锁内重入——image_broadcast v2 卡死教训）；等待留 detached
// 线程（200ms 轮询 + 5s 心跳含 Start 计数，image_broadcast v3 等待诊断同款）。
//
// 落位：src/media/cdn_url_exchange.{h,cpp}；接线见本补丁目录 README §7。
//
#include "media/cdn_url_exchange.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "media/cdn_cache.h"
#include "logger.h"
#include "runtime/main_thread_executor.h"
#include "offset.h"
#include "utils/address_utils.h"
#include "utils/file_utils.h"
#include "vtable_helper.h"

// 配置目录可被宿主单测覆写；Android 构建走默认值（与 raw_pb_broadcast 同款）
#ifndef GIF_URL_CONFIG_DIR
#define GIF_URL_CONFIG_DIR "/storage/emulated/0/Android/data/com.tencent.wework/cache"
#endif

namespace {

// ===== offset.h 追加项（本文件局部定义，与 cdn_cache.cpp v4.1 观测 hook 同风格，免改 offset.h）=====
#define OFFSET_SVC_MGR_GET_FILE_SERVICE    (48)        // ServiceManager 虚表：GetFileService（0x11EA680 L38-39）nativeCdnUploadFileOnPathSceneAndSubScene
#define OFFSET_FILE_SVC_CDN_UPLOAD_ON_PATH (192)       // FileService 虚表：CdnUploadFileOnPath(请求*, &result_ref, &progress_ref)
#define OFFSET_SVC_MGR_GET_COLLECTION_SVC  (376)       // ServiceManager 虚表：GetCollectionProtocol 服务（0x10B6838 L40-41）
#define OFFSET_COLLECTION_GET_BACKEND      (112)       // Collection 服务虚表：GetCollectionBackend
#define OFFSET_BACKEND_GET_GIF_URL         (440)       // CollectionBackend 虚表：GetGifUrlByFileId(fileId&,aeskey&,md5&,ClosureRef&)
#define OFFSET_CDN_UPLOAD_REQ_CTOR         (0x15CF4C8) // 上传请求 pbc ctor（64B 布局见文件头）
#define OFFSET_CDN_UPLOAD_REQ_DTOR         (0x15CF6A0) // 上传请求 pbc dtor（内部处理哨兵值，勿手撕）
#define OFFSET_CLOSURE_CTOR                (0x6D35F44) // 闭包 ctor(closure, invoke, destroy)：+0=1/+8/+16/+24=noop
#define OFFSET_CLOSURE_REF_RELEASE         (0x6D36044) // ClosureRef 释放（null 安全；计数归零→调 closure+16）

constexpr const char *kReqPath    = GIF_URL_CONFIG_DIR "/url.req";
constexpr const char *kSentPath   = GIF_URL_CONFIG_DIR "/url.req.sent";
constexpr const char *kResultPath = GIF_URL_CONFIG_DIR "/url.result";
constexpr const char *kDefaultPath = "/storage/emulated/0/test.jpg";  // url.req 无路径行时的缺省图片（与 §1 图片测试同源）

constexpr int kUploadWaitTimeoutMs = 60 * 1000;
constexpr int kWaitPollStepMs      = 200;
constexpr int kHeartbeatEvery      = 25;   // 每 25 次(5s)打一条心跳

// 当前线程 ID：Android/Linux 用 SYS_gettid；宿主单测(macOS) 用 thread_selfid
pid_t current_tid() {
#if defined(__APPLE__)
    return static_cast<pid_t>(syscall(SYS_thread_selfid));
#else
    return syscall(SYS_gettid);
#endif
}

// 主线程判定：Linux/Android 主线程 gettid == getpid；macOS 宿主单测直接放行
bool is_main_thread() {
#if defined(__APPLE__)
    return true;
#else
    return syscall(SYS_gettid) == getpid();
#endif
}

std::string read_text_file(const char *path) {
    const std::vector<uint8_t> bytes = read_binary_file(path);
    return std::string(bytes.begin(), bytes.end());
}

// 读取企微侧 libc++ std::string（SSO：首字节低 bit=1 为长串 {cap@0,size@8,ptr@16}）
// ——与 cdn_cache.cpp read_native_string 同源拷贝（本模块不依赖其内部符号）
std::string read_native_string(const void *obj) {
    if (!obj) return {};
    const auto *p = static_cast<const uint8_t *>(obj);
    if ((p[0] & 1) == 0) {                                    // 短串：len = p[0]>>1，数据内联 +1
        const size_t len = p[0] >> 1;
        return std::string(reinterpret_cast<const char *>(p + 1), len);
    }
    const size_t len = *reinterpret_cast<const size_t *>(p + 8);         // 长串：size@+8
    const char *data = *reinterpret_cast<const char *const *>(p + 16);   //       ptr@+16
    if (!data || len == 0 || len > (4u << 20)) return {};                // 防御性上限 4MB
    return std::string(data, len);
}
    constexpr size_t kClosureSize = 0x40;       // +0 计数=1 / +8 invoke / +16 destroy / +24 noop / +32 捕获
    constexpr size_t kClosureSlotCapture = 32;  // 自有捕获指针槽（框架仅触碰 +0/+8/+16/+24）
    constexpr uint32_t kHasBitPath = 1u;        // CdnUploadRequest.has_bits：path 置位（bit0）
    constexpr uint32_t kHasBitsSceneSubsceneType = 0x38u; // has_bits：scene/subscene/type 三 int（bit3-5）
    // 构造框架通用闭包：ctor 置计数/invoke/destroy/noop，捕获指针写入 +32 自有槽
    uint8_t *make_closure(void *invoke, void *destroy, void *capture) {
        auto closure_ctor = reinterpret_cast<void *(*)(void *, void *, void *)>(get_abs_addr(
                "libwework_framework.so", OFFSET_CLOSURE_CTOR));
        auto *closure = static_cast<uint8_t *>(operator new(kClosureSize));
        std::memset(closure, 0, kClosureSize);
        closure_ctor(closure, invoke, destroy);
        *reinterpret_cast<void **>(closure + kClosureSlotCapture) = capture;
        return closure;
    }
// ===== 上传请求结构（sub_15CF4C8 ctor 实测布局，64 字节）=====
struct alignas(8) CdnUploadRequest {
    uintptr_t vtable;      // +0  off_A537C48（ctor 写入）
    uintptr_t internal;    // +8  ctor 置 0
    uint32_t has_bits;     // +16 |=1(path) |=0x38(三 int)
    uint32_t _pad20;       // +20
    uintptr_t path_field;  // +24 pbc string*（未置位 = so_base+0xAB487F8 哨兵值）
    uintptr_t str_field_b; // +32 哨兵（本请求不置位）
    uintptr_t str_field_c; // +40 哨兵（本请求不置位）
    int32_t scene;         // +48（App 表情流 = 1）
    int32_t subscene;      // +52（App 表情流 = 42）
    int32_t type;          // +56（App 表情流 = -1）
    int32_t _pad60;        // +60
};
static_assert(sizeof(CdnUploadRequest) == 64, "CdnUploadRequest 布局漂移：重核 sub_15CF4C8");

// ===== 兑换完成闭包（0x40；+32 起为自有捕获——框架仅触碰 +0/+8/+16/+24）=====
struct GifUrlCapture {
    std::string local_path;   // 诊断用
    std::string file_id;      // 诊断用（兑换凭证）
    GifUrlResult on_result;   // 业务回调（可为空 = 仅日志 + url.result）
};

// ===== 回调模式（vt+192 真回调闭包，可选）=====
// 完成回调（wxcdn 任务线程）读三元组后**直接投递**主线程兑换任务——无共享状态、
// 无轮询、不经 [cdn_cache] 取结果（无缓存快路；观测 hook 在场时的缓存填充由
// hook 自行完成，与本链路无关）。lambda 自持三元组与业务回调，invoke 返回后
// 闭包即可析构。
struct UploadCbCapture {
    std::string local_path;    // 诊断 + 兑换任务路径
    GifUrlResult on_result;    // 业务回调（透传给兑换闭包；可为空 = 仅日志 + url.result）
    GifUrlFullResult on_result_full;  // 全量模式业务回调（非空 → 兑换回调补上传三元组一次交付）
};
struct UploadProgressCapture {
    std::string local_path;    // 进度日志标识
};

// invoke ABI（sub_2860C54 实证）：(closure, int* errcode, str1*, str2*, str3*)
// errcode 按指针；三个 libc++ string 归 deferred 闭包所有——只读，绝不释放；
// 返回值仅回填 x0，框架不消费。在框架回调线程执行：无 JNI、无锁、无释放。
// 三串语义（2026-10-16 真机定名，与 Java 调用方 i8r.b 日志同名）：
//   s1 = new url   ——主图 lasting URL（wework.qpic.cn/wwpic3az/<资产ID>_<令牌>_<时间戳>/0）
//   s2 = cover url ——封面/缩略图 URL（独立资产ID、同签发时间戳）
//   s3 = md5       ——md5 回显（与上传三元组的 md5 逐字节一致）
int64_t my_gif_url_invoke(void *closure, int *errcode, void *s1, void *s2, void *s3) {
    if (!closure) return 0;
    auto *cap = *reinterpret_cast<GifUrlCapture **>(static_cast<uint8_t *>(closure) + 32);
    const int code = errcode ? *errcode : -1;
    const std::string new_url = read_native_string(s1);    // s1：new url（主图）
    const std::string cover_url = read_native_string(s2);  // s2：cover url（封面/缩略图）
    const std::string md5 = read_native_string(s3);        // s3：md5 回显

    LOGI("[gif_url] ★ 兑换完成 errcode=%d s1=[%s] s2=[%s] s3=[%s] (fileid=%.24s...)",
         code, new_url.c_str(), cover_url.c_str(), md5.c_str(),
         cap ? cap->file_id.c_str() : "");

    // 结果落盘（s1/s2/s3 键与三串语义见上方 ABI 注）：adb pull url.result 即取（覆盖写，最新一次为准）
    if (FILE *f = std::fopen(kResultPath, "w")) {
        std::fprintf(f, "# gif_url_exchange result (epoch=%lld)\nerrcode=%d\ns1=%s\ns2=%s\ns3=%s\n",
                     static_cast<long long>(std::time(nullptr)), code,
                     new_url.c_str(), cover_url.c_str(), md5.c_str());
        std::fclose(f);
    } else {
        LOGW("[gif_url] url.result 写入失败: %s (%s)", kResultPath, std::strerror(errno));
    }

    if (cap && cap->on_result) {
        cap->on_result(code, new_url, cover_url, md5);
    }
    return 0;
}

// destroy ABI（sub_6D36044 归零路径实证）：单参 = closure 本身；负责释放捕获与闭包内存
void my_gif_url_destroy(void *closure) {
    if (!closure) return;
    auto *cap = *reinterpret_cast<GifUrlCapture **>(static_cast<uint8_t *>(closure) + 32);
    delete cap;
    operator delete(closure);
    LOGD("[gif_url] 完成闭包已释放（引用计数归零，全链路生命周期闭环）");
}

// ===== 回调模式闭包（vt+192 真回调；JNI 非空分支同构伪造）=====
// 前置声明：invoke 直接投递兑换（定义在下方服务链/兑换段）
bool call_get_gif_url_by_fileid(const CdnUploadResult &cdn, const std::string &path,
                                GifUrlResult on_result);
// result invoke ABI（sub_11EF96C 实证）：(closure, bool* ok, str* fileid, str* aeskey,
// str* md5)——ok 按指针传入（转发槽解引用后交 marshaller sub_11EF814 作 onResult
// 布尔）；三串归上传任务结果所有——只读勿释放。wxcdn 任务线程执行：读串 + 日志 +
// **直接投递主线程兑换任务**（lambda 自持三元组/路径/业务回调，invoke 返回后闭包
// 即可析构）——零原生调用、无共享状态、不经 [cdn_cache]。
int64_t my_upload_result_invoke(void *closure, const uint8_t *ok, void *fileid, void *aeskey, void *md5) {
    if (!closure) return 0;
    auto *cap = *reinterpret_cast<UploadCbCapture **>(static_cast<uint8_t *>(closure) + 32);
    const bool success = ok && ((*ok & 1) != 0);
    const std::string fid = read_native_string(fileid);
    const std::string aes = read_native_string(aeskey);
    const std::string m5 = read_native_string(md5);

    LOGI("[gif_url] [cb] 上传完成回调到达: ok=%d fileid=%.32s... aes=%s md5=%s",
         success ? 1 : 0, fid.c_str(), aes.c_str(), m5.c_str());

    if (!success || fid.empty()) {
        LOGE("[gif_url] [cb] 上传回调报失败(ok=0)或票据为空，本轮中止（不经兑换）");
        if (cap && cap->on_result_full) {
            // 全量模式失败也交付（errcode=-1 合成，三串为空）：调用方无须另设失败侦测
            GifUrlFullResult full = std::move(cap->on_result_full);
            full(-1, fid, aes, m5, "", "", "");
        }
        return 0;
    }

    // 回调直达：从本回调线程投递主线程兑换任务（executor.post 跨线程安全，与既有
    // detached 线程投递同机制）；三元组/路径/业务回调拷入 lambda，闭包析构不受影响
    std::string local_path;
    GifUrlResult on_result;
    GifUrlFullResult on_result_full;
    if (cap) {
        local_path = cap->local_path;
        on_result = std::move(cap->on_result);
        on_result_full = std::move(cap->on_result_full);
    }
    MainThreadExecutor::getInstance().post(
        [local_path, fid, aes, m5, on_result, on_result_full]() {
            LOGD("[gif_url] 兑换任务开始 tid=%d (期望=主线程pid=%d)", current_tid(), getpid());
            const CdnUploadResult cdn{fid, aes, m5, 0};
            if (on_result_full) {
                // 全量模式：包装兑换回调——到达时补齐上传三元组，七元组一次交付
                GifUrlResult wrapped = [fid, aes, m5, on_result_full](
                        int code, const std::string &new_url, const std::string &cover_url,
                        const std::string &md5) {
                    on_result_full(code, fid, aes, m5, new_url, cover_url, md5);
                };
                if (!call_get_gif_url_by_fileid(cdn, local_path, wrapped)) {
                    LOGE("[gif_url] 兑换请求发出失败（服务链解析/闭包构造，见上方日志）");
                }
            } else if (!call_get_gif_url_by_fileid(cdn, local_path, on_result)) {
                LOGE("[gif_url] 兑换请求发出失败（服务链解析/闭包构造，见上方日志）");
            }
        });
    return 0;
}

// destroy ABI（sub_11EFA14 同构）：单参 = closure；释放捕获与闭包内存
void my_upload_result_destroy(void *closure) {
    if (!closure) return;
    auto *cap = *reinterpret_cast<UploadCbCapture **>(static_cast<uint8_t *>(closure) + 32);
    delete cap;
    operator delete(closure);
    LOGD("[gif_url] [cb] 上传完成闭包已释放（引用计数归零）");
}

// progress invoke ABI（sub_11EFB7C 实证）：(closure, str* ident, int64* cur, int64* total)
// —— 两计数器按指针（marshaller sub_11EFA58 解引用后传 Java (String,J,J)）
int64_t my_upload_progress_invoke(void *closure, void *ident, const int64_t *cur, const int64_t *total) {
    if (!closure) return 0;
    const std::string id = read_native_string(ident);
    const int64_t c = cur ? *cur : 0;
    const int64_t t = total ? *total : 0;
    LOGD("[gif_url] [cb] 上传进度 %s: %lld/%lld (%.1f%%)",
         id.c_str(), static_cast<long long>(c), static_cast<long long>(t),
         t > 0 ? 100.0 * static_cast<double>(c) / static_cast<double>(t) : 0.0);
    return 0;
}

void my_upload_progress_destroy(void *closure) {
    if (!closure) return;
    auto *cap = *reinterpret_cast<UploadProgressCapture **>(static_cast<uint8_t *>(closure) + 32);
    delete cap;
    operator delete(closure);
    LOGD("[gif_url] [cb] 进度闭包已释放（引用计数归零）");
}

// ===== ServiceManager 解析（conv_service.cpp 同款链）=====
// nativeGetServiceManager(0,0,profile_wrapper) = unwrap(wrapper)→vt[264]()，
// 返回【裸 ServiceManager】——nativeGetConversationService 直接 a3->vt[40] 实证，
// 勿再解引用；profile_wrapper 为临时包装，用毕即删（镜像 conv_service.cpp）。
uintptr_t resolve_service_manager() {
    const uintptr_t base = get_module_base("libwework_framework.so");
    if (!base) {
        LOGE("[gif_url] libwework_framework.so 未加载");
        return 0;
    }
    auto get_current_profile =
        reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(base + OFFSET_NATIVE_GET_CURRENT_PROFILE);
    auto get_service_manager =
        reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t)>(base + OFFSET_NATIVE_GET_SERVICE_MANAGER);

    const uintptr_t profile_wrapper = get_current_profile(0, 0);
    if (!profile_wrapper) {
        LOGE("[gif_url] nativeGetCurrentProfile 返回空（未登录/Profile 未就绪？）");
        return 0;
    }
    const uintptr_t sm = get_service_manager(0, 0, profile_wrapper);
    operator delete(reinterpret_cast<void *>(profile_wrapper));
    if (!sm) {
        LOGE("[gif_url] nativeGetServiceManager 返回空");
    }
    return sm;
}

// ===== 半程一：独立上传（主线程执行）=====
// 不调 JNI 导出 0x11EA680——那是 Java→native 编组壳（IsSameObject 空回调判定 +
// GetStringUTFChars 路径编组 + 闭包包装），其最后一步才是 FileService::vt+192。
// 本函数逐行复刻该 native 半程：vt+192(请求*, 空result_ref, 空progress_ref)，
// 与 JNI 空回调分支（反编译 L40/L58 置零 ref + L81-104 请求构造/分发/析构）
// 完全同构；完成信号不经伪造回调，由 cdn_cache OnUploadFinish 观测 hook 捕获。
bool trigger_standalone_upload(const std::string &path, int scene, int subscene, int type) {
    const uintptr_t base = get_module_base("libwework_framework.so");
    const uintptr_t sm = resolve_service_manager();
    if (!base || !sm) return false;

    const uintptr_t file_service = CallVMethodByOffset<uintptr_t>(sm, OFFSET_SVC_MGR_GET_FILE_SERVICE);
    if (!file_service) {
        LOGE("[gif_url] ServiceManager→vt+%d 取 FileService 返回空", OFFSET_SVC_MGR_GET_FILE_SERVICE);
        return false;
    }

    auto req_ctor    = reinterpret_cast<void (*)(CdnUploadRequest *)>(base + OFFSET_CDN_UPLOAD_REQ_CTOR);
    auto req_dtor    = reinterpret_cast<void (*)(CdnUploadRequest *)>(base + OFFSET_CDN_UPLOAD_REQ_DTOR);

    // 请求构造：ctor → has_bits|=1 → 路径字段（未置位走 new-拷贝主路径，已置位兜底 assign）
    // → 三 int → has_bits|=0x38 —— 顺序与 JNI 完全一致
    CdnUploadRequest req;
    req_ctor(&req);
    req.has_bits |= 1u;
    req.path_field = reinterpret_cast<uintptr_t>(new std::string(path));
    req.scene = scene;
    req.subscene = subscene;
    req.type = type;
    req.has_bits |= 0x38u;

    // 空回调 refs：JNI 空回调分支的原生形态（两个零值 qword 槽位地址）
    uintptr_t result_ref   = 0;
    uintptr_t progress_ref = 0;
    CallVMethodByOffset<void>(file_service, OFFSET_FILE_SVC_CDN_UPLOAD_ON_PATH,
                              &req, &result_ref, &progress_ref);
    req_dtor(&req);

    LOGI("[gif_url] 独立上传已触发: path=%s scene=%d subscene=%d type=%d (完成经 [cdn_cache] 观测)",
         path.c_str(), scene, subscene, type);
    return true;
}

// ===== 半程一（回调模式）：独立上传 + 真 result/progress 闭包（主线程执行）=====
// 与 trigger_standalone_upload 同构，唯两 ref 换为伪造闭包——即 JNI 非空回调分支的
// 原生形态（L43-55/L61-75：new 0x40 → sub_6D35F44 → +32 捕获 → 槽位=裸指针）。
// 引用流转（sub_6D3603C 纯赋值实证无 addref）：ctor 计数=1 归我方槽位 → vt+192
// 实现侧同步 addref 拷入上传任务 → 调毕 sub_6D36044 释放自有引用 → 任务完成
// invoke(+8) 后释放归零 → destroy(+16) 收尾（与兑换闭包同一协议，真机已验）。
// on_result_full 非空 = 全量模式（cb 变体传空）：上传三元组随兑换回调一次交付。
bool trigger_standalone_upload_cb(const std::string &path, int scene, int subscene, int type,
                                  GifUrlResult on_result, GifUrlFullResult on_result_full) {
    const uintptr_t sm = resolve_service_manager();
    if (!sm) return false;

    const uintptr_t file_service = CallVMethodByOffset<uintptr_t>(sm,
                                                                  OFFSET_SVC_MGR_GET_FILE_SERVICE);
    if (!file_service) {
        LOGE("[gif_url] ServiceManager→vt+%d 取 FileService 返回空",
             OFFSET_SVC_MGR_GET_FILE_SERVICE);
        return false;
    }

    auto req_ctor = reinterpret_cast<void (*)(CdnUploadRequest *)>(get_abs_addr(
            "libwework_framework.so", OFFSET_CDN_UPLOAD_REQ_CTOR));
    auto req_dtor = reinterpret_cast<void (*)(CdnUploadRequest *)>(get_abs_addr(
            "libwework_framework.so",
            OFFSET_CDN_UPLOAD_REQ_DTOR));

    CdnUploadRequest req;
    req_ctor(&req);
    req.has_bits |= kHasBitPath;
    req.path_field = reinterpret_cast<uintptr_t>(new std::string(path));
    req.scene = scene;
    req.subscene = subscene;
    req.type = type;
    req.has_bits |= kHasBitsSceneSubsceneType;

    // 伪造 result/progress 闭包（0x40；+32 起自有捕获——框架仅触碰 +0/+8/+16/+24）
    auto ref_release = reinterpret_cast<void (*)(uintptr_t *)>(get_abs_addr(
            "libwework_framework.so", OFFSET_CLOSURE_REF_RELEASE));

    auto *result_closure = make_closure(
            reinterpret_cast<void *>(my_upload_result_invoke),
            reinterpret_cast<void *>(my_upload_result_destroy),
            new UploadCbCapture{path, std::move(on_result), std::move(on_result_full)});
    auto *prog_closure = make_closure(
            reinterpret_cast<void *>(my_upload_progress_invoke),
            reinterpret_cast<void *>(my_upload_progress_destroy),
            new UploadProgressCapture{path});

    // 真回调 refs：调毕释放自有引用，框架侧 addref 持有至任务完成（JNI L108-109 同款）
    uintptr_t result_ref = reinterpret_cast<uintptr_t>(result_closure);
    uintptr_t progress_ref = reinterpret_cast<uintptr_t>(prog_closure);
    CallVMethodByOffset<void>(file_service, OFFSET_FILE_SVC_CDN_UPLOAD_ON_PATH,
                              &req, &result_ref, &progress_ref);
    req_dtor(&req);
    ref_release(&result_ref);
    ref_release(&progress_ref);

    LOGD("[gif_url] 独立上传(回调模式)已触发: path=%s scene=%d subscene=%d type=%d (完成经原生回调直达)",
         path.c_str(), scene, subscene, type);
    return true;
}

// ===== 半程二：服务端换 URL（主线程执行）=====
// SM→vt+376(Collection 服务)→vt+112(Backend)→vt+440(fileId&,aeskey&,md5&,ref&)。
// 伪造 0x40 完成闭包经框架自有 ctor 构造；自有引用调毕即释放（sub_6D36044），
// 框架侧 AddRef 持有至 CGI 完成后由 deferred 闭包释放链归零 → destroy 收尾。
bool call_get_gif_url_by_fileid(const CdnUploadResult &cdn, const std::string &path,
                                GifUrlResult on_result) {
    const uintptr_t sm = resolve_service_manager();
    if (!sm) return false;

    const uintptr_t collection_svc = CallVMethodByOffset<uintptr_t>(sm,
                                                                    OFFSET_SVC_MGR_GET_COLLECTION_SVC);
    if (!collection_svc) {
        LOGE("[gif_url] ServiceManager→vt+%d 取 Collection 服务返回空",
             OFFSET_SVC_MGR_GET_COLLECTION_SVC);
        return false;
    }
    const uintptr_t backend = CallVMethodByOffset<uintptr_t>(collection_svc,
                                                             OFFSET_COLLECTION_GET_BACKEND);
    if (!backend) {
        LOGE("[gif_url] Collection 服务→vt+%d 取 CollectionBackend 返回空",
             OFFSET_COLLECTION_GET_BACKEND);
        return false;
    }

    auto ref_release = reinterpret_cast<void (*)(uintptr_t *)>(get_abs_addr(
            "libwework_framework.so", OFFSET_CLOSURE_REF_RELEASE));

    auto *closure = make_closure(reinterpret_cast<void *>(my_gif_url_invoke),
                                 reinterpret_cast<void *>(my_gif_url_destroy),
                                 new GifUrlCapture{path, cdn.file_id, std::move(on_result)});

    std::string file_id(cdn.file_id);
    std::string aes_key(cdn.aes_key);
    std::string md5(cdn.md5);

    uintptr_t cb_ref = reinterpret_cast<uintptr_t>(closure);  // = sub_6D3603C(&ref, closure) 纯移动
    CallVMethodByOffset<void>(backend, OFFSET_BACKEND_GET_GIF_URL,
                              &file_id, &aes_key, &md5, &cb_ref);
    ref_release(&cb_ref);  // 释放自有引用；框架 AddRef 侧归零时 destroy 自动收尾

    LOGD("[gif_url] 兑换请求已发出: fileid=%.32s... aes=%.16s... md5=%.16s... (fileid全长=%zu)",
         cdn.file_id.c_str(), cdn.aes_key.c_str(), cdn.md5.c_str(), cdn.file_id.size());
    return true;
}

} // namespace

void upload_and_exchange_gif_url(const std::string &local_path,
                                 int scene, int subscene, int type,
                                 GifUrlResult onResult) {
    if (local_path.empty()) return;

    // 主线程护栏：initialize() 绑定【当前线程】的 Looper，必须由主线程调
    // （my_nativeSend 实测主线程；image_broadcast v3.1-3 教训）
    if (!is_main_thread()) {
        LOGE("[gif_url] 调用线程不是主线程(tid=%d, pid=%d)，中止", current_tid(), getpid());
        return;
    }
    static bool s_executor_ok = false;
    if (!s_executor_ok) {
        s_executor_ok = MainThreadExecutor::getInstance().initialize();
    }

    // 版本横幅：部署核对就认这一行（看不到 = 跑的还是旧库）
    LOGI("[gif_url] active: 独立上传+服务端换URL, path=%s scene=%d subscene=%d type=%d executor=%s",
         local_path.c_str(), scene, subscene, type, s_executor_ok ? "ok" : "FAIL");
    if (!s_executor_ok) {
        LOGE("[gif_url] executor 初始化失败，中止本次流程（不回退直发——陌生线程原生调用是 v2 死亡路径）");
        return;
    }

    // 编排线程：等待/心跳留后台；两个原生半程各投递主线程执行（排在 orig_nativeSend
    // 之后，避免发送管线锁内重入）。投递任务全部按值捕获 + shared_ptr，自包含。
    std::thread([local_path, scene, subscene, type, onResult]() {
        // ① 缓存命中（本进程此前传过且未过 TTL）→ 免上传直接兑换
        CdnUploadResult cdn;
        const bool initially_cached = cdn_cache_get(local_path, cdn);
        bool cached = initially_cached;

        if (!cached) {
            // 捕获 hook 未安装 → 永远等不到完成信号，直接中止（启动日志应有
            // "[cdn_cache] capture hooks: OnUploadFinish=ok"）
            if (!cdn_cache_hooks_installed()) {
                LOGE("[gif_url] cdn_cache 捕获 hook 未安装——无法观测上传完成，中止"
                     "（检查 init_send_hook 内 init_cdn_capture_hooks 是否执行）");
                return;
            }

            // ② 主线程触发独立上传；失败标志供等待循环提前退出
            auto trigger_failed = std::make_shared<std::atomic_bool>(false);
            MainThreadExecutor::getInstance().post(
                [local_path, scene, subscene, type, trigger_failed]() {
                    LOGD("[gif_url] 上传触发任务开始 tid=%d (期望=主线程pid=%d)", current_tid(), getpid());
                    if (!trigger_standalone_upload(local_path, scene, subscene, type)) {
                        trigger_failed->store(true);
                    }
                });

            // ③ 后台等待（绝不阻塞主线程）：200ms 轮询 + 5s 心跳 + Start 计数诊断
            int polled = 0;
            for (int waited = 0; waited < kUploadWaitTimeoutMs && !cached; waited += kWaitPollStepMs) {
                cached = cdn_cache_get(local_path, cdn);
                if (cached) break;
                if (trigger_failed->load()) {
                    LOGE("[gif_url] 上传触发失败（见上方日志），中止等待");
                    return;
                }
                if (++polled % kHeartbeatEvery == 0) {
                    LOGD("[gif_url] 等待上传结果 %d/%dms, Start 次数=%d",
                         waited + kWaitPollStepMs, kUploadWaitTimeoutMs,
                         cdn_upload_start_count(local_path));
                }
                usleep(kWaitPollStepMs * 1000);
            }
            if (!cached) {
                const int starts = cdn_upload_start_count(local_path);
                LOGE("[gif_url] 等待上传结果超时(%dms), Start 次数=%d → %s",
                     kUploadWaitTimeoutMs, starts,
                     starts == 0 ? "上传从未开始(请求被拒/管道未放行)"
                                 : "上传已开始未完成(网络慢或完成回调未达)");
                return;
            }
        }

        LOGI("[gif_url] 上传结果就绪%s: fileid=%.32s... aes=%s md5=%s",
             initially_cached ? "(缓存复用，免上传)" : "(本次独立上传)", cdn.file_id.c_str(),
             cdn.aes_key.c_str(), cdn.md5.c_str());

        // ④ 主线程发起服务端兑换；完成回调在框架线程到达（★ 横幅 + url.result）
        MainThreadExecutor::getInstance().post(
            [local_path, cdn, onResult]() {
                LOGD("[gif_url] 兑换任务开始 tid=%d (期望=主线程pid=%d)", current_tid(), getpid());
                if (!call_get_gif_url_by_fileid(cdn, local_path, onResult)) {
                    LOGE("[gif_url] 兑换请求发出失败（服务链解析/闭包构造，见上方日志）");
                }
            });
    }).detach();
}

// ===== 回调模式编排（与 upload_and_exchange_gif_url 平行；观测模式保持不动）=====
// 纯回调链路：主线程仅投递“带真回调的触发”（排在 orig_nativeSend 之后）；上传
// 完成回调（wxcdn 线程）读三元组后直接投递主线程兑换任务——无共享状态、无轮询、
// 不经 [cdn_cache] 取结果（无缓存快路；观测 hook 在场时的缓存填充由 hook 自行
// 完成，与本链路无关）。
void upload_and_exchange_gif_url_cb(const std::string &local_path,
                                    int scene, int subscene, int type,
                                    GifUrlResult onResult) {
    if (local_path.empty()) return;

    // 主线程护栏（与观测模式同理：executor 绑定当前线程 Looper）
    if (!is_main_thread()) {
        LOGE("[gif_url] 调用线程不是主线程(tid=%d, pid=%d)，中止", current_tid(), getpid());
        return;
    }
    static bool s_executor_ok_cb = false;
    if (!s_executor_ok_cb) {
        s_executor_ok_cb = MainThreadExecutor::getInstance().initialize();
    }

    LOGI("[gif_url] active: 独立上传(回调)+服务端换URL, path=%s scene=%d subscene=%d type=%d executor=%s",
         local_path.c_str(), scene, subscene, type, s_executor_ok_cb ? "ok" : "FAIL");
    if (!s_executor_ok_cb) {
        LOGE("[gif_url] executor 初始化失败，中止本次流程（不回退直发——陌生线程原生调用是 v2 死亡路径）");
        return;
    }

    // 主线程投递触发；上传完成后由 [cb] 完成回调直接投递兑换任务（见 my_upload_result_invoke）
    MainThreadExecutor::getInstance().post(
        [local_path, scene, subscene, type, onResult]() {
            LOGD("[gif_url] [cb] 上传触发任务开始 tid=%d (期望=主线程pid=%d)", current_tid(), getpid());
            if (!trigger_standalone_upload_cb(local_path, scene, subscene, type,
                                              onResult, GifUrlFullResult())) {
                LOGE("[gif_url] 上传触发失败（见上方日志），本轮中止（完成回调不会到来）");
            }
        });
}

// ===== 全量变体编排（图片→表情链路；与 cb 变体同一纯回调链）=====
// 上传完成回调（wxcdn 线程）读三元组 → 投递主线程兑换 → 兑换回调包装补齐三元组 →
// 七元组（上传三元组 + 兑换三串）一次交付 onResult。无共享状态、无轮询、不经 [cdn_cache]。
void upload_and_exchange_gif_url_full(const std::string &local_path,
                                      int scene, int subscene, int type,
                                      GifUrlFullResult onResult) {
    if (local_path.empty()) return;

    // 主线程护栏（与 cb 变体同理：executor 绑定当前线程 Looper）
    if (!is_main_thread()) {
        LOGE("[gif_url] 调用线程不是主线程(tid=%d, pid=%d)，中止", current_tid(), getpid());
        return;
    }
    static bool s_executor_ok_full = false;
    if (!s_executor_ok_full) {
        s_executor_ok_full = MainThreadExecutor::getInstance().initialize();
    }

    LOGI("[gif_url] active: 独立上传(回调·全量)+服务端换URL, path=%s scene=%d subscene=%d type=%d executor=%s",
         local_path.c_str(), scene, subscene, type, s_executor_ok_full ? "ok" : "FAIL");
    if (!s_executor_ok_full) {
        LOGE("[gif_url] executor 初始化失败，中止本次流程（不回退直发——陌生线程原生调用是 v2 死亡路径）");
        return;
    }

    // 主线程投递触发；上传完成后由 [cb] 完成回调直接投递兑换任务（见 my_upload_result_invoke）
    MainThreadExecutor::getInstance().post(
        [local_path, scene, subscene, type, onResult]() {
            LOGD("[gif_url] [cb] 上传触发任务开始 tid=%d (期望=主线程pid=%d)", current_tid(), getpid());
            if (!trigger_standalone_upload_cb(local_path, scene, subscene, type,
                                              GifUrlResult(), onResult)) {
                LOGE("[gif_url] 上传触发失败（见上方日志），本轮中止（完成回调不会到来）");
            }
        });
}

bool try_exchange_gif_url_from_files() {
    if (::access(kReqPath, R_OK) != 0) {
        return false;  // url.req 不存在 = 模式未启用，调用方走原逻辑
    }

    const std::string text = read_text_file(kReqPath);
    std::string path;
    int scene = 1, subscene = 42, type = -1;   // App 表情流缺省值（ylj.d0 实测）
    bool have_path = false;

    // 回调模式指令：任一注释行含 mode=cb / mode=callback → 上传完成改走原生回调闭包
    // （不经 [cdn_cache] 观测；指令行须带 '#'，纯路径行不误判）
    bool callback_mode = false;
    for (size_t scan = 0; scan < text.size();) {
        const size_t nl = text.find('\n', scan);
        const std::string l = text.substr(scan, (nl == std::string::npos ? text.size() : nl) - scan);
        scan = (nl == std::string::npos) ? text.size() : nl + 1;
        if (l.find('#') != std::string::npos &&
            (l.find("mode=cb") != std::string::npos || l.find("mode=callback") != std::string::npos)) {
            callback_mode = true;
            break;
        }
    }
    if (callback_mode) {
        LOGI("[gif_url] url.req 回调模式指令命中——上传完成经原生回调闭包直达（不经 [cdn_cache] 观测）");
    }

    // 解析：'#' 注释；首个非注释行 = 路径（整行收下，容忍空格；缺省 kDefaultPath）；
    // 第二个非注释行可写 "scene,subscene,type"
    std::string line;
    size_t pos = 0;
    auto trim = [](std::string &s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    };
    while (pos <= text.size() && !have_path) {
        const size_t nl = text.find('\n', pos);
        line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        path = line;
        have_path = true;
    }
    // 第二个非注释行：可选三 int
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        int s2 = 0, s3 = 0, s4 = 0;
        if (std::sscanf(line.c_str(), "%d ,%d ,%d", &s2, &s3, &s4) == 3) {
            scene = s2; subscene = s3; type = s4;
            LOGI("[gif_url] url.req 自定义上传参数: scene=%d subscene=%d type=%d", scene, subscene, type);
        } else {
            LOGW("[gif_url] url.req 第二行无法解析为 \"scene,subscene,type\"（收到 \"%s\"），"
                 "沿用缺省 1,42,-1", line.c_str());
        }
        break;  // 只看第一个非注释行
    }

    if (!have_path || path.empty()) {
        // 无路径行 = 缺省图片（空/纯注释 url.req 即“上传默认图换URL”，与 §1 测试图同源）
        path = kDefaultPath;
        LOGI("[gif_url] url.req 无路径行——使用缺省图片 %s", kDefaultPath);
    }

    // 一次性触发：装载成功即改名，防手动连发重复上传；失败仅告警不阻断
    if (std::rename(kReqPath, kSentPath) != 0) {
        LOGW("[gif_url] %s 改名失败(%s)——下次触发会再来一轮，注意手动删除",
             kReqPath, std::strerror(errno));
    }

    // 空回调 = 仅 ★ 日志 + url.result 落盘（文件驱动的全部交付物）
    if (callback_mode) {
        upload_and_exchange_gif_url_cb(path, scene, subscene, type, GifUrlResult());
    } else {
        upload_and_exchange_gif_url(path, scene, subscene, type, GifUrlResult());
    }
    return true;
}
