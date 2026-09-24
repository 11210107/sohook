//
// image_broadcast.cpp — 同一张图片群发多联系人，CDN 只上传一次（方案1：fileid 免上传）
//
// ★ v4.0（2026-09-22 第四轮排障定稿）：图片 content_type 7 → 14 ★
//  真机实测（14:31 日志）：v3.1 管线五项目标全部达标（主线程发送 tid==pid、发送
//  全部返回、单次上传 Start=1、捕获成功 fileid/aes/md5、复用零上传），但
//  on_result 三连失败：首个接收者 210（消息实际已送达），复用×2 86。
//  86 根因（静态+实测闭环）：
//   * 复用模型 type 7 ∉ CDN 类型集合（sub_5104AB4(7)=0）→ case 10 LABEL_243
//     → LABEL_482 直发 —— 携带的是【另一个消息上传的 fileid】，不经服务器
//     续期直接投递，服务端拒绝（86）。
//   * type 14 ∈ CDN 集合（sub_5104AB4(14)=1，runtime 实测）→ state 15
//     "renew cdn file"（chat_task.cpp:2682）按 fileid 续期、零文件字节 ——
//     正是原生转发（sub_1655398 保留 type 14）的合法复用路径。
//  → v4：首传与复用统一 content_type=14。case 13 上传通道同样按
//    sub_5104AB4(msgtype) 分流（L2342-2345）：14 → DoUploadBinWithCdn ——
//    首传票据确定走 CDN 通道，与复用续期同通道，全链路票据类型一致。
//  → 部署自证：外层 Message.content_type=Tag7，14 序列化为 "38 0e"（v3 为
//    38 07）。send_one_* 会打 [v4] 模型头日志，一眼核对新库真在跑。
//
// ★ v3.1（2026-09-22 第三次排障定稿）★
//  v3 结论不变：发送原语（vtable+848）必须回【主线程】执行，编排/等待留后台线程。
//  v3.1 在 v3 基础上补 4 个实战缺口（每个都来自真机踩坑）：
//   1) 版本横幅：每次广播打 "[image_broadcast] v3.1 active" —— 部署核对一眼可辨。
//      （踩坑实录：工程里的 image_broadcast.cpp 曾悄悄停留在 v2，旧日志被当成
//       v3 行为反复分析；没有横幅就无法确认"新库真的在跑"。）
//   2) executor 初始化失败 → 直接【中止广播】，绝不在编排线程上直发。
//      v3 曾保留"回退为后台直发"作兜底 —— 那是 v2 的死亡路径（会毒死整条发送
//      管道），宁可少发广播也不能卡死用户自己的消息（orig 路径不受影响）。
//   3) initialize() 主线程护栏：my_nativeSend 实测在主线程被调（tid==pid）。
//      若意外跑在别的线程，ALooper_prepare(0) 会"成功"创建一个永远没人驱动的
//      Looper，post 的任务永远不执行 —— 静默失败比崩溃难查十倍，入口直接拒绝。
//   4) 捕获 hook 未安装（init_cdn_capture_hooks 没跑/失败）→ 跳过 60s 空等，
//      全部联系人立刻常规发送（消息必达，各自上传，传输层 md5 去重兜底），
//      并打 ERROR 级日志指路。
//
// ★ v3（2026-09-22 第二次卡死复盘，根治线程模型问题）★
//  * 发送原语必须回到【主线程】执行。sendModelMsg.md §0/§4 证据：vtable+848 在
//    调用线程上【同步】完成"本地落库(sub_244BD6C 先插本地消息,状态=发送中) +
//    异步任务入队(sub_2452804)"后才返回；ChatTask/上传全在其后的 IO 线程。
//  * 基线版本（可用）的三连发全部跑在主线程：my_nativeSend 内、orig_nativeSend
//    之前、顺序执行。v2 改成 detached 后台线程后，vtable+848 在陌生线程上停摆，
//    且停摆时持有消息管道核心锁 —— 连主线程 orig_nativeSend 的正常发送也被
//    拖死在同一把锁上（实测：用户自己的第一条消息也永远转圈）。
//  * v3 修复：编排（60s 等待/节流）留在后台线程绝不阻塞主线程；每次真正的
//    send_model_message 经 MainThreadExecutor 投递到主线程执行 —— 与基线线程
//    模型完全一致，且主线程单队列天然把我们的发送排到 orig_nativeSend 之后。
//  * 等待诊断：每 5s 打一条心跳（含 CdnUploadTask::Start 计数）；超时日志区分
//    count=0（上传从未开始：落库后管道/网络门控未放行）与 count>=1（已开始未
//    完成：网络慢或完成回调未达），直接指路。
//
// 落位：src/image_broadcast.cpp —— 用本文件整体替换工程内旧版后重新编译；
//       v4 必须连同 message/message_pb.cpp（content_type=14）一起拷，
//       cdn_cache.cpp / cdn_cache.h 与 v3.1 相同（未改动可不重拷）。
//       三个文件都在本补丁目录，直接 cp 即可（见 README §1）。
//
#include "image_broadcast.h"
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "cdn_cache.h"
#include "logger.h"
#include "main_thread_executor.h"
#include "message_sender.h"
#include "message/message_pb.h"
#include "utils/file_utils.h"

namespace {

// 等待首个接收者 CDN 上传结果的超时（大图/弱网兜底；超时后其余联系人回退逐个上传）
constexpr int kCdnWaitTimeoutMs = 60 * 1000;
constexpr int kWaitPollStepMs   = 200;   // 轮询步进
constexpr int kHeartbeatEvery   = 25;    // 每 25 次(5s)打一条心跳日志

pid_t current_tid() { return syscall(SYS_gettid); }

// v4 部署自证：外层 Message.content_type = Tag 7，14 序列化为 "38 0e"（v3 为 38 07）
void log_pb_head(const char *tag, const std::vector<uint8_t> &pb) {
    const size_t n = pb.size() < 4 ? pb.size() : 4;
    const unsigned int b0 = n > 0 ? pb[0] : 0;
    const unsigned int b1 = n > 1 ? pb[1] : 0;
    const unsigned int b2 = n > 2 ? pb[2] : 0;
    const unsigned int b3 = n > 3 ? pb[3] : 0;
    LOGI("[v4] %s模型: size=%zu head=%02x %02x %02x %02x (期望 38 0e = content_type 14)",
         tag, pb.size(), b0, b1, b2, b3);
}

// v4 复用发送：content_type=14 + fileid → state 15 "renew cdn file" 服务器续期（零上传）
// （v3 用 type 7 → LABEL_482 直发，跨消息 fileid 不续期 → 实测 errcode 86 被拒）
void send_one_reuse(uint64_t conv_id, const std::string &path, const CdnUploadResult &cdn,
                    uint64_t size, int w, int h, int ld_w, int ld_h,
                    const BroadcastProgress &onProgress, const BroadcastResult &onResult) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    std::vector<uint8_t> pb = generate_image_reuse_pb(name, size, w, h, ld_w, ld_h,
                                                      cdn.file_id, cdn.aes_key, cdn.md5);
    log_pb_head("复用", pb);
    send_model_message(conv_id, MSG_TYPE_IMAGE, std::move(pb), onProgress, onResult);
}

// 常规发送：模型带本地路径（触发上传）；v4 同用 content_type=14（CDN 通道票据，
// case 13 按 sub_5104AB4(14)=1 → DoUploadBinWithCdn，与复用续期同通道）
void send_one_normal(uint64_t conv_id, const std::string &path,
                     uint64_t size, int w, int h, int ld_w, int ld_h,
                     const BroadcastProgress &onProgress, const BroadcastResult &onResult) {
    std::vector<uint8_t> pb = generate_image_message_pb(path, w, h, size, false, "", "", ld_w, ld_h);
    log_pb_head("常规", pb);
    send_model_message(conv_id, MSG_TYPE_IMAGE, std::move(pb), onProgress, onResult);
}

} // namespace

void send_image_to_contacts(const std::string &image_path,
                            const std::vector<uint64_t> &receiver_conv_ids,
                            BroadcastProgress onProgress,
                            BroadcastResult onResult) {
    if (image_path.empty() || receiver_conv_ids.empty()) return;

    // v3.1-3 主线程护栏：initialize() 绑定【当前线程】的 Looper，必须由主线程调
    if (current_tid() != getpid()) {
        LOGE("[image_broadcast] 调用线程不是主线程(tid=%d, pid=%d)，中止广播",
             current_tid(), getpid());
        return;
    }

    // MainThreadExecutor::initialize() 必须在主线程调用——本函数由 my_nativeSend
    // （主线程）触发，此处仍在主线程；已初始化则跳过，失败则下次广播重试
    static bool s_main_executor_ok = false;
    if (!s_main_executor_ok) {
        s_main_executor_ok = MainThreadExecutor::getInstance().initialize();
    }

    // v3.1-1 版本横幅（v4.0 同机制）：部署核对就认这一行（看不到 = 跑的还是旧库！）
    LOGI("[image_broadcast] v4.0 active: 主线程投递 + content_type=14(CDN续期), executor=%s",
         s_main_executor_ok ? "ok" : "FAIL");

    if (!s_main_executor_ok) {
        // v3.1-2 绝不回退到编排线程直发（v2 死亡路径）
        LOGE("[image_broadcast] executor 初始化失败，中止本次广播"
             "（用户经 orig_nativeSend 的消息不受影响）");
        return;
    }

    // 编排线程：只做等待/节流/决策，真正的发送投递到主线程
    std::thread([image_path, receiver_conv_ids, onProgress, onResult]() {
        // v3.1-4 捕获 hook 未安装 → 不做 60s 空等，全部常规发送（消息必达）
        const bool hooks_ok = cdn_cache_hooks_installed();
        LOGI("[image_broadcast] 广播开始: 接收者=%zu 个, path=%s, 捕获hook=%s",
             receiver_conv_ids.size(), image_path.c_str(),
             hooks_ok ? "已安装" : "未安装(全部常规发送)");
        if (!hooks_ok) {
            LOGE("[image_broadcast] init_cdn_capture_hooks 未生效——复用不可用。"
                 "启动日志应出现 \"[cdn_cache] capture hooks: OnUploadFinish=ok\"");
        }

        uint64_t image_size = 0;
        uint32_t w = 0, h = 0;
        if (!get_image_info(image_path, image_size, w, h)) {
            LOGE("[image_broadcast] 解析本地图片参数失败: %s", image_path.c_str());
            return;
        }
        const int ld_w = static_cast<int>(w) * 3 / 4;
        const int ld_h = static_cast<int>(h) * 3 / 4;

        // 进程内已缓存（上一轮广播上传过且未过 TTL）→ 全部复用，一次都不传
        CdnUploadResult cdn;
        bool reuse_ready = cdn_cache_get(image_path, cdn);
        int upload_waits = 0;

        for (size_t i = 0; i < receiver_conv_ids.size(); i++) {
            usleep(50000); // 群发节流：给企微底层 TaskQueue/网络线程让出缓冲（沿用原经验值）

            if (!reuse_ready) {
                reuse_ready = cdn_cache_get(image_path, cdn); // 上传可能在等待期间完成
            }

            const bool use_reuse = reuse_ready;
            const uint64_t rid = receiver_conv_ids[i];
            LOGD("[image_broadcast] 第 %zu/%zu 个接收者 %llu → %s(投递主线程执行)",
                 i + 1, receiver_conv_ids.size(), static_cast<unsigned long long>(rid),
                 use_reuse ? "复用发送(14续期)" : "常规发送(上传)");

            // 发送任务：捕获全部按值，自包含，主线程异步执行时与编排线程无共享状态
            auto send_task = [rid, image_path, cdn, use_reuse, image_size, w, h,
                              ld_w, ld_h, onProgress, onResult,
                              seq = i + 1, total = receiver_conv_ids.size()]() {
                // v3.1: 执行线程自证（tid 应等于进程 pid = 主线程）+ 前后日志精确定位停摆点
                LOGD("[image_broadcast] 发送任务#%zu/%zu 开始 tid=%d",
                     seq, total, current_tid());
                if (use_reuse) {
                    send_one_reuse(rid, image_path, cdn, image_size,
                                   static_cast<int>(w), static_cast<int>(h), ld_w, ld_h,
                                   onProgress, onResult);
                } else {
                    send_one_normal(rid, image_path, image_size,
                                    static_cast<int>(w), static_cast<int>(h), ld_w, ld_h,
                                    onProgress, onResult);
                }
                LOGD("[image_broadcast] 发送任务#%zu/%zu 已返回", seq, total);
            };

            MainThreadExecutor::getInstance().post(std::move(send_task));

            if (hooks_ok && !use_reuse && i + 1 < receiver_conv_ids.size() && upload_waits == 0) {
                // 只等第一次上传的结果（在后台线程等，绝不阻塞主线程）；
                // 带 5s 心跳 + Start 计数，超时结论直接写进日志
                upload_waits++;
                int polled = 0;
                for (int waited = 0; waited < kCdnWaitTimeoutMs; waited += kWaitPollStepMs) {
                    if (cdn_cache_get(image_path, cdn)) { reuse_ready = true; break; }
                    if (++polled % kHeartbeatEvery == 0) {
                        LOGD("[image_broadcast] 等待上传结果 %d/%dms, Start 次数=%d",
                             waited + kWaitPollStepMs, kCdnWaitTimeoutMs,
                             cdn_upload_start_count(image_path));
                    }
                    usleep(kWaitPollStepMs * 1000);
                }
                if (!reuse_ready) {
                    const int starts = cdn_upload_start_count(image_path);
                    LOGE("[image_broadcast] 等待 CDN 上传结果超时(%dms), Start 次数=%d → %s",
                         kCdnWaitTimeoutMs, starts,
                         starts == 0 ? "上传从未开始(落库后管道/网络门控未放行)"
                                     : "上传已开始未完成(网络慢或完成回调未达)");
                }
            }
        }

        LOGI("[image_broadcast] 广播完成: 接收者=%zu 个, CdnUploadTask::Start 次数=%d (期望=1)",
             receiver_conv_ids.size(), cdn_upload_start_count(image_path));
    }).detach();
}
