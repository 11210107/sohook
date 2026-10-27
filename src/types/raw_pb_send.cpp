//
// raw_pb_broadcast.cpp — 直接 PB 流群发编排（视频号/小程序等捕获消息的重放）
//
// 数据流：
//   my_nativeSend 触发(主线程)
//     └─ try_send_raw_pb_from_files
//          ├─ pb.hex / pb.bin 均不存在 → 返回 false，调用方走原测试逻辑
//          ├─ 两格式并存 → 拒绝本轮（不猜意图，防两次触发先后群发两份内容）
//          ├─ 装载 pb.hex（hex 文本，'#' 注释 + #keep-all）或 pb.bin（原始二
//          │   进制 PB 字节，无注释通道固定走默认清洗）+ receivers.txt
//          ├─ sanitize_message_pb_for_send（默认）或 validate（#keep-all）
//          ├─ rename 装载来源 → *.sent（一次性触发防重）
//          └─ send_raw_pb_to_contacts
//               ├─ [主线程] MainThreadExecutor.initialize()（一次）+ 横幅自证
//               └─ [后台线程] 编排循环：节流 50ms/条
//                    └─ post→[主线程] send_model_message(清洗后的 PB)
//                           → vtable+848(主线程同步: 落库+入队) → IO 线程 ChatTask
//                              → content_type=13/78 为自包含内容，无上传分支
//
#include "types/raw_pb_send.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "logger.h"
#include "runtime/main_thread_executor.h"
#include "send/message_sender.h"
#include "message/raw_pb.h"
#include "utils/file_utils.h"
#include "utils/protocol_utils.h"
#include "config/receivers.h"

// 配置目录可被宿主单测覆写；Android 构建走默认值
#ifndef RAW_PB_CONFIG_DIR
#define RAW_PB_CONFIG_DIR "/storage/emulated/0/Android/data/com.tencent.wework/cache"
#endif

namespace {

// 当前线程 ID：Android/Linux 用 SYS_gettid；宿主单测(macOS) 用 thread_selfid
pid_t current_tid() {
#if defined(__APPLE__)
    return static_cast<pid_t>(syscall(SYS_thread_selfid));
#else
    return syscall(SYS_gettid);
#endif
}

// 主线程判定：Linux/Android 主线程 gettid == getpid；macOS 的 selfid 与 pid
// 不同号，宿主单测环境直接放行（该护栏仅对 Android 目标有意义）
bool is_main_thread() {
#if defined(__APPLE__)
    return true;
#else
    return syscall(SYS_gettid) == getpid();
#endif
}

constexpr const char *kPbHexPath     = RAW_PB_CONFIG_DIR "/pb.hex";
constexpr const char *kPbHexSentPath = RAW_PB_CONFIG_DIR "/pb.hex.sent";
constexpr const char *kPbBinPath     = RAW_PB_CONFIG_DIR "/pb.bin";      // 原始二进制 PB（如 file/image_message.bin 直接 adb push）
constexpr const char *kPbBinSentPath = RAW_PB_CONFIG_DIR "/pb.bin.sent";
constexpr const char *kReceiversPath = RAW_PB_CONFIG_DIR "/receivers.txt";

std::string read_text_file(const char *path) {
    const std::vector<uint8_t> bytes = read_binary_file(path);
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

void send_raw_pb_to_contacts(const std::vector<uint8_t> &raw_pb,
                             const std::vector<uint64_t> &receiver_conv_ids,
                             BroadcastProgress onProgress,
                             BroadcastResult onResult) {
    if (raw_pb.empty() || receiver_conv_ids.empty()) {
        return;
    }

    // 主线程护栏：MainThreadExecutor::initialize() 绑定【当前线程】的 Looper，
    // 必须由主线程调（image_broadcast v3.1-3 教训）
    if (!is_main_thread()) {
        LOGE("[raw_pb] 调用线程不是主线程(tid=%d, pid=%d)，中止群发", current_tid(), getpid());
        return;
    }

    static bool s_main_executor_ok = false;
    if (!s_main_executor_ok) {
        s_main_executor_ok = MainThreadExecutor::getInstance().initialize();
    }

    const int content_type = rawpb::extract_content_type(raw_pb);
    LOGI("[raw_pb] active: 直接PB流群发 size=%zu content_type=%d 接收者=%zu executor=%s",
         raw_pb.size(), content_type, receiver_conv_ids.size(),
         s_main_executor_ok ? "ok" : "FAIL");
    if (!s_main_executor_ok) {
        // v3.1-2 教训：绝不回退到编排线程直发（v2 死亡路径）
        LOGE("[raw_pb] executor 初始化失败，中止本次群发（用户经 orig_nativeSend 的消息不受影响）");
        return;
    }

    // 编排线程：只做节流；每次真正的 send_model_message 投递到主线程执行。
    // 发送任务捕获全部按值（含整份 PB 拷贝），自包含、与编排线程无共享状态。
    std::thread([raw_pb, receiver_conv_ids, content_type, onProgress, onResult]() {
        for (size_t i = 0; i < receiver_conv_ids.size(); i++) {
            usleep(50000); // 群发节流：给企微底层 TaskQueue/网络线程让出缓冲（沿用原经验值）

            const uint64_t rid = receiver_conv_ids[i];
            auto send_task = [raw_pb, rid, content_type, onProgress, onResult,
                              seq = i + 1, total = receiver_conv_ids.size()]() {
                // 执行线程自证（tid 应等于进程 pid = 主线程）
                LOGD("[raw_pb] 发送任务#%zu/%zu 开始 tid=%d (conv=%llu)",
                     seq, total, current_tid(), static_cast<unsigned long long>(rid));
                // msg_type 参数为死代码（README §11：模型 PB 经 ParseFromMem
                // 全权解析），传 content_type 仅为日志可读
                // dump_protobuf_hex(raw_pb);
                send_model_message(rid, content_type, raw_pb, onProgress, onResult);
                LOGD("[raw_pb] 发送任务#%zu/%zu 已返回", seq, total);
            };
            MainThreadExecutor::getInstance().post(std::move(send_task));
        }
        LOGI("[raw_pb] 群发完成: 接收者=%zu 个", receiver_conv_ids.size());
    }).detach();
}

bool try_send_raw_pb_from_files(BroadcastProgress onProgress, BroadcastResult onResult) {
    const bool hex_exists = ::access(kPbHexPath, R_OK) == 0;
    const bool bin_exists = ::access(kPbBinPath, R_OK) == 0;
    LOGD("hex_exists: %d bin_exists: %d", hex_exists,bin_exists);
    if (!hex_exists && !bin_exists) {
        LOGE("PB config files not found, raw PB mode disabled");
        return false; // 配置文件不存在 = raw PB 模式未启用，调用方走原逻辑
    }

    // 两种输入格式并存 = 有遗留文件：不猜用户意图，直接拒绝本轮（文件都
    // 保留）——否则连续两次触发会把两份不同内容先后群发出去
    if (hex_exists && bin_exists) {
        LOGE("[raw_pb] pb.hex 与 pb.bin 同时存在——请删除其一后重新触发。本次不发送: %s / %s",
             kPbHexPath, kPbBinPath);
        return true;
    }

    // 输入文件存在：本次触发即由 raw 模式接管——即使后续失败也不回落图片测试
    // 路径（防止同一次触发双发），失败原因全部写进日志
    std::vector<uint8_t> raw;
    bool keep_all = false;
    const char *src_path  = kPbHexPath;
    const char *sent_path = kPbHexSentPath;
    if (hex_exists) {
        const std::string hex_text = read_text_file(kPbHexPath);
        keep_all = hex_text.find("#keep-all") != std::string::npos;
        if (!rawpb::parse_hex_stream(hex_text, raw)) {
            LOGE("[raw_pb] pb.hex 解析失败，本次不发送（文件保留未改名，修正后重新触发即可）");
            return true;
        }
    } else {
        // pb.bin：原始二进制 PB 字节（捕获工具落盘的 .bin 直接 adb push，
        // 如工程 file/image_message.bin）。二进制没有注释通道 → 固定走
        // 默认清洗；要 #keep-all 原样重放请先转 hex：xxd -p pb.bin > pb.hex
        src_path  = kPbBinPath;
        sent_path = kPbBinSentPath;
        raw = read_binary_file(kPbBinPath);
        if (raw.empty()) {
            LOGE("[raw_pb] pb.bin 为空或读取失败: %s", kPbBinPath);
            return true;
        }
    }

    const std::string recv_text = read_text_file(kReceiversPath);
    if (recv_text.empty()) {
        LOGE("[raw_pb] receivers.txt 不存在或为空: %s", kReceiversPath);
        return true;
    }
    std::vector<uint64_t> receivers;
    if (!parse_receivers(recv_text, receivers) || receivers.empty()) {
        LOGE("[raw_pb] receivers.txt 无合法会话 ID，本次不发送");
        return true;
    }

    std::vector<uint8_t> pb;
    if (keep_all) {
        if (!rawpb::validate_message_pb(raw)) {
            LOGE("[raw_pb] #keep-all 原样重放：wire 校验失败，拒绝发送残缺 PB");
            return true;
        }
        pb = raw;
        LOGI("[raw_pb] #keep-all 模式：原样重放 %zu 字节（含原消息身份/状态字段，风险自担，来源 %s）",
             pb.size(), src_path);
    } else {
        size_t stripped = 0;
        pb = rawpb::sanitize_message_pb_for_send(raw, &stripped);
        if (pb.empty()) {
            LOGE("[raw_pb] 出站清洗失败（PB 残缺/截断），本次不发送");
            return true;
        }
        LOGI("[raw_pb] 装载完成: 原始 %zu 字节 → 出站 %zu 字节（剔除 %zu 个身份/状态字段，来源 %s）",
             raw.size(), pb.size(), stripped, src_path);
    }

    // 一次性触发：装载成功即把来源文件改名，防手动连发导致重复群发；失败仅告警不阻断
    if (std::rename(src_path, sent_path) != 0) {
        LOGW("[raw_pb] %s 改名失败(%s)——下次触发会再发一轮，注意手动删除",
             src_path, std::strerror(errno));
    }
    send_raw_pb_to_contacts(pb, receivers, std::move(onProgress), std::move(onResult));
    return true;
}
