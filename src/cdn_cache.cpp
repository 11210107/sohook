//
// cdn_cache.cpp — CDN 上传结果捕获与复用缓存（方案1：fileid 免上传）
//
// 捕获点证据链（详见 ../forward_msg.md、../file_msg_upload_cdn.md §9）：
//  * OnUploadFinish 签名 (this, errcode, seq, fileid*, aeskey*, md5*, result*)，
//    a4/a5/a6 三个 std::string* 的语义由 cdn_bypass.js 运行时实测确认
//    （a4=wwftn 长票据 fileid、a5=aeskey、a6=md5）；
//  * CdnUploadTask 布局（§9.9）：+88 = 本地文件路径 string（inline）、+176 = file_size；
//  * 上传通道按【内部 msgtype(task+584)】分流（case 13，sub_5106E10.c L2342-2345：
//    CheckCdnType≠0 → CDN sub_5112730，否则 → FTN sub_51130B4）；两通道完成
//    都汇入 OnUploadFinish（唯一汇聚点）；
//  * -21005 传输层去重路径同样回调到此处（errcode=0、fileid 为已存在的票据），
//    天然与缓存值一致。
//
// ★★★ ABI 铁律（2026-09-22 实测踩坑修复）★★★
//  反编译签名：
//    __int64 sub_2B87DEC(__int64 a1)                       // Start，返回 __int64
//    __int64 sub_2B88B70(a1..a7)                            // OnUploadFinish，返回 __int64，
//                                                          // 且把结果回调的返回值原样 return
//  hook 替身若声明为 void：转发原函数后 x0 的返回值被丢弃，hook 返回时 x0 为
//  垃圾（常见为 LOGD 里 __android_log_print 的返回值=打印字符数）。经 vtable
//  发射的调用方拿到伪"非 0 返回"→ 按"启动失败"处理 → 上传完成监听装配被跳过
//  → ChatTask 永远等不到写回 → 消息卡死在"发送中"（实测症状：第一条图片
//  消息一直转圈）。修复：替身与转发指针的返回类型必须与原生函数一致（int64_t），
//  并把原函数返回值原样 return。
//
// 落位：src/cdn_cache.cpp
//
#include "cdn_cache.h"
#include <chrono>
#include <map>
#include <mutex>
#include <unistd.h>

#include "dobby.h"
#include "logger.h"
#include "offset.h"

namespace {

// fileid 时效保守值（state 15 续期逻辑的存在说明 CDN/FTN fileid 有有效期；
// 同一次广播间隔秒级，远小于该值；跨广播复用超时后自动回退为重新上传）
constexpr int64_t kCacheTtlMs = 30 * 60 * 1000;

std::mutex g_mutex;
std::map<std::string, CdnUploadResult> g_results;
std::map<std::string, int> g_start_counts;
bool g_hooks_installed = false;

// ★ 返回类型 int64_t 必须与原生 __int64 完全一致（见文件头 ABI 铁律）
typedef int64_t (*CdnOnUploadFinish)(void *, int64_t, void *, void *, void *, void *, void *);
CdnOnUploadFinish orig_on_upload_finish = nullptr;

typedef int64_t (*CdnUploadStart)(void *);
CdnUploadStart orig_cdn_upload_start = nullptr;

// 读取企微侧 libc++ std::string（SSO：首字节低 bit=1 为长串 {cap@0,size@8,ptr@16}）
// 与 NDK libc++ 布局一致，纯内存读取，无跨模块调用
std::string read_native_string(const void *obj) {
    if (!obj) return {};
    const auto *p = static_cast<const uint8_t *>(obj);
    if ((p[0] & 1) == 0) {                                    // 短串：len = p[0]>>1，数据内联 +1
        size_t len = p[0] >> 1;
        return std::string(reinterpret_cast<const char *>(p + 1), len);
    }
    const size_t len = *reinterpret_cast<const size_t *>(p + 8);         // 长串：size@+8
    const char *data = *reinterpret_cast<const char *const *>(p + 16);   //       ptr@+16
    if (!data || len == 0 || len > (4u << 20)) return {};                // 防御性上限 4MB
    return std::string(data, len);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// CdnUploadTask::OnUploadFinish(this, errcode, seq, fileid*, aeskey*, md5*, result*)
// 纯观测 hook：成功则记录缓存；errcode!=0 时留痕便于排障；原函数返回值原样透传
int64_t my_on_upload_finish(void *task, int64_t errcode, void *seq, void *file_id,
                            void *aes_key, void *md5, void *result) {
    LOGD("[cdn_cache] OnUploadFinish enter errcode=%lld", static_cast<long long>(errcode));
    if (errcode == 0 && task) {
        const std::string path = read_native_string(static_cast<uint8_t *>(task) + 88);
        CdnUploadResult r;
        r.file_id = read_native_string(file_id);
        r.aes_key = read_native_string(aes_key);
        r.md5 = read_native_string(md5);
        if (!path.empty() && !r.file_id.empty()) {
            r.captured_at_ms = now_ms();
            std::lock_guard<std::mutex> lk(g_mutex);
            g_results[path] = r;
            LOGI("[cdn_cache] 捕获上传成功 path=%s fileid=%.32s... aes=%s md5=%s",
                 path.c_str(), r.file_id.c_str(), r.aes_key.c_str(), r.md5.c_str());
        }
    } else if (errcode != 0) {
        LOGW("[cdn_cache] OnUploadFinish 上传失败 errcode=%lld（原函数照常放行）",
             static_cast<long long>(errcode));
    }
    if (orig_on_upload_finish) {
        return orig_on_upload_finish(task, errcode, seq, file_id, aes_key, md5, result);
    }
    return 0;
}

// CdnUploadTask::Start(this) —— 计数量化"只上传一次"，返回值原样透传
int64_t my_cdn_upload_start(void *task) {
    if (task) {
        const std::string path = read_native_string(static_cast<uint8_t *>(task) + 88);
        if (!path.empty()) {
            std::lock_guard<std::mutex> lk(g_mutex);
            const int n = ++g_start_counts[path];
            LOGD("[cdn_cache] CdnUploadTask::Start path=%s count=%d", path.c_str(), n);
        }
    }
    if (orig_cdn_upload_start) {
        return orig_cdn_upload_start(task);
    }
    return 0;
}

// ============================================================================
// v4.1 观测 hook（2026-09-22 第五轮）：ChatTask 状态机追踪
//  背景：真机实测外层 content_type=7 的【带路径】图片消息，其内部 msgtype
//  (task+584)=14 —— 外层类型与内部 msgtype 之间存在 native 侧映射（映射点未定位；
//  sohook 侧 create_message_pure_native_ptr 的 type 参数是死代码——写入 impl+24 的
//  语句已注释，模型 PB 经 ParseFromMem 全权解析，PB content_type 是 Message 类型
//  的唯一来源）。本组 hook 逐次打印 task+584 与状态转移，用于：
//   1) 验证映射（用户要求的"打印日志看下"）；
//   2) 复用发送是否真走 state 15 "renew cdn file"（chat_task.cpp:2682）；
//   3) 解码 on_result 错误码来源（60 是否经 case 23 取消检查 → state 25 路径，
//      "[CancelMsg] Message cancel sending DoSendMessage!"，chat_task.cpp:3374）。
//  反编译签名（ABI 铁律：返回类型与原生完全一致）：
//    void     sub_5106E10(__int64 a1)   // 状态机分发器 ★ void 返回，替身也必须是 void
//    __int64  sub_5104AB4(int a1)       // CheckCdnType：{13,14,15,16,17,23,29,78}→1
// ============================================================================
#define OFFSET_CHAT_TASK_DISPATCH (0x5106E10)  // sub_5106E10（局部定义，免改 offset.h）
#define OFFSET_CHECK_CDN_TYPE     (0x5104AB4)  // sub_5104AB4

typedef void (*ChatTaskDispatch)(void *);
ChatTaskDispatch orig_chat_task_dispatch = nullptr;

typedef int64_t (*CheckCdnTypeFn)(int);
CheckCdnTypeFn orig_check_cdn_type = nullptr;

// 状态机分发入口：每次状态转移打一条（task+12=state、+584=msgtype、+260=错误码）
void my_chat_task_dispatch(void *task) {
    if (task) {
        const auto *p = static_cast<const uint8_t *>(task);
        LOGD("[chat_task] dispatch task=%p state=%d msgtype=%d err=%d",
             task,
             *reinterpret_cast<const int *>(p + 12),
             *reinterpret_cast<const int *>(p + 584),
             *reinterpret_cast<const int *>(p + 260));
    }
    if (orig_chat_task_dispatch) {
        orig_chat_task_dispatch(task);
    }
}

// CheckCdnType：LABEL_243（续期/直发分流）与 case 13（上传通道分流）的共同判定
int64_t my_check_cdn_type(int type) {
    const int64_t r = orig_check_cdn_type ? orig_check_cdn_type(type) : 0;
    LOGD("[chat_task] CheckCdnType(%d)=%lld", type, static_cast<long long>(r));
    return r;
}

} // namespace

bool init_cdn_capture_hooks(uintptr_t so_base) {
    if (g_hooks_installed) return true;
    if (so_base == 0) return false;

    void *finish_addr = reinterpret_cast<void *>(so_base + OFFSET_CDN_ON_UPLOAD_FINISH);
    const int r1 = DobbyHook(finish_addr, (dobby_dummy_func_t) my_on_upload_finish,
                             (dobby_dummy_func_t *) &orig_on_upload_finish);
    void *start_addr = reinterpret_cast<void *>(so_base + OFFSET_CDN_UPLOAD_START);
    const int r2 = DobbyHook(start_addr, (dobby_dummy_func_t) my_cdn_upload_start,
                             (dobby_dummy_func_t *) &orig_cdn_upload_start);

    g_hooks_installed = (r1 == 0 && r2 == 0);
    LOGI("[cdn_cache] capture hooks: OnUploadFinish=%s(%p) Start=%s(%p)",
         r1 == 0 ? "ok" : "FAIL", finish_addr, r2 == 0 ? "ok" : "FAIL", start_addr);

    // v4.1 观测 hook（纯日志；失败只影响诊断能力，不影响捕获/复用功能，
    // 也不计入 g_hooks_installed —— 广播等待只依赖捕获 hook）
    void *dispatch_addr = reinterpret_cast<void *>(so_base + OFFSET_CHAT_TASK_DISPATCH);
    const int r3 = DobbyHook(dispatch_addr, (dobby_dummy_func_t) my_chat_task_dispatch,
                             (dobby_dummy_func_t *) &orig_chat_task_dispatch);
    void *check_type_addr = reinterpret_cast<void *>(so_base + OFFSET_CHECK_CDN_TYPE);
    const int r4 = DobbyHook(check_type_addr, (dobby_dummy_func_t) my_check_cdn_type,
                             (dobby_dummy_func_t *) &orig_check_cdn_type);
    LOGI("[cdn_cache] trace hooks: Dispatch=%s(%p) CheckCdnType=%s(%p)",
         r3 == 0 ? "ok" : "FAIL", dispatch_addr, r4 == 0 ? "ok" : "FAIL", check_type_addr);
    return g_hooks_installed;
}

bool cdn_cache_get(const std::string &local_path, CdnUploadResult &out) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const auto it = g_results.find(local_path);
    if (it == g_results.end()) return false;
    if (now_ms() - it->second.captured_at_ms > kCacheTtlMs) {
        g_results.erase(it);   // 过期：视为未命中，广播层会回退为重新上传
        return false;
    }
    out = it->second;
    return true;
}

void cdn_cache_put(const std::string &local_path, const CdnUploadResult &r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_results[local_path] = r;
}

bool cdn_cache_wait(const std::string &local_path, int timeout_ms, CdnUploadResult &out) {
    constexpr int kStepMs = 200;
    for (int waited = 0; waited < timeout_ms; waited += kStepMs) {
        if (cdn_cache_get(local_path, out)) return true;
        usleep(kStepMs * 1000);
    }
    return cdn_cache_get(local_path, out);
}

int cdn_upload_start_count(const std::string &local_path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const auto it = g_start_counts.find(local_path);
    return it == g_start_counts.end() ? 0 : it->second;
}

bool cdn_cache_hooks_installed() {
    // 广播层据此决定是否做 60s 等待：未安装则缓存永远不可能命中
    return g_hooks_installed;
}
