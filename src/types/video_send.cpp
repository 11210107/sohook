//
// video_send.cpp — 视频文件消息发送（contentType=5，参数直传版）
//
// 证据链：
//  * contentType=5 = 视频族 wire 型（sub_5104AE8 表 {5,17,22,23,51,103}，
//    trace/sendModelMsg.md §7.1；2026-10-22 真机捕获出站样例外层 38 05 实证）
//  * content PB 模板 = 2026-10-22 真机捕获 275B 出站样例 content(10) 260B
//    （nativeSetInfo 本地态：url=本地 mp4 路径、raw_url 同值、CDN 三元组与
//    105..107/203..205 全空；宿主单测 video_content_test 对其逐字节回归）
//  * 元数据 = utils/file_utils.cpp get_video_info（moov box 级解析：size 物理
//    字节、宽高已按 tkhd 旋转矩阵换向、时长 ms；宿主 vmeta_test）
//  * 组包 = message/message_pb.cpp build_video_content_pb（生成类
//    sohook::VideoMessage，字段 1,3,4,5,6,7,202 升序、缺省不编码）
//
// 线程模型：入口本地读文件 + 组包（无原生服务调用）+ executor 主线程护栏
// （首调拉起 MainThreadExecutor——参数直传入口无 gif_url_exchange 前段顺
// 路初始化；2026-10-22 link_send 真机实证无护栏时 post 静默丢弃、群发零
// 投递）；executor 就绪后任意线程可调。群发 = detached 节流线程，每发经
// MainThreadExecutor 投递主线程（发送原语主线程亲和）——rawpb v3.1+ /
// emotion 同款编排，无共享状态。
//
// 落位：src/types/video_send.{h,cpp}；
//
#include "types/video_send.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "logger.h"
#include "runtime/main_thread_executor.h"
#include "send/message_sender.h"
#include "message/message_pb.h"
#include "utils/file_utils.h"
#include "utils/address_utils.h"
#include "config/receivers.h"
#include "config/req_config.h"

// 配置目录与 emoji.req / link.req / pb.hex 同一处（可被宿主单测覆写；Android 构建走默认值）
#ifndef VIDEO_CONFIG_DIR
#define VIDEO_CONFIG_DIR "/storage/emulated/0/Android/data/com.tencent.wework/cache"
#endif

namespace {

constexpr int kVideoContentType = 5;   // 视频族 wire 型（捕获样例 38 05 实证）

constexpr const char *kVideoReqPath  = VIDEO_CONFIG_DIR "/video.req";
constexpr const char *kVideoSentPath = VIDEO_CONFIG_DIR "/video.req.sent";
constexpr const char *kReceiversPath = VIDEO_CONFIG_DIR "/receivers.txt";  // video.req 未带 ID 时回退（与 rawpb/emotion 共用）

// video.req 合法字段（小写；path/preview_img 是别名）——文法与严格性由
// utils/req_utils.cpp 统一承载（link.req 共用同一实现）
const std::vector<std::string> kVideoReqKeys = {
        "video", "path", "preview", "preview_img", "receivers"};

// 当前线程 ID：Android/Linux 用 SYS_gettid；宿主单测(macOS) 用 thread_selfid
// （emotion_send.cpp 同款拷贝，本模块不依赖其内部符号）
pid_t current_tid() {
#if defined(__APPLE__)
    return static_cast<pid_t>(syscall(SYS_thread_selfid));
#else
    return syscall(SYS_gettid);
#endif
}

// 主线程判定：Linux/Android 主线程 gettid == getpid；macOS 的 selfid 与 pid
// 不同号，宿主单测环境直接放行（stub executor 恒就绪，护栏不会真触达）
bool is_main_thread() {
#if defined(__APPLE__)
    return true;
#else
    return syscall(SYS_gettid) == getpid();
#endif
}

} // namespace

bool send_video_to_contacts(const std::string &video_path,
                            const std::vector<uint64_t> &receiver_conv_ids,
                            const std::string &preview_img_path,
                            std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                            std::function<void(int, uint64_t)> onResult) {
    if (video_path.empty()) {
        LOGE("[video_send] 视频路径为空，本轮不发送");
        return false;
    }
    if (receiver_conv_ids.empty()) {
        LOGE("[video_send] 接收者列表为空，本轮不发送");
        return false;
    }

    // 主线程护栏：MainThreadExecutor::initialize() 绑定【当前线程】的 Looper，
    // 必须由主线程调（image_broadcast v3.1-3 / raw_pb_broadcast 同款判据）。
    // 参数直传入口不经 gif_url_exchange 前段，executor 不会"顺路"被初始化——
    // 首调必须自己拉起（2026-10-22 link_send 真机实证同款 bug：无护栏时
    // post 静默丢弃，"群发完成"横幅照打但零投递）
    MainThreadExecutor &executor = MainThreadExecutor::getInstance();
    if (!executor.isInitialized()) {
        if (!is_main_thread()) {
            LOGE("[video_send] executor 未初始化且调用线程非主线程(tid=%d)，"
                 "无法在此初始化（Looper 将绑到错误线程），本轮不发送", current_tid());
            return false;
        }
        if (!executor.initialize()) {
            LOGE("[video_send] executor 初始化失败，本轮不发送");
            return false;
        }
        LOGI("[video_send] MainThreadExecutor 初始化完成（主线程护栏首调）");
    }

    // 元数据（moov box 级：物理字节/宽高（已换向）/时长 ms）
    uint64_t video_size = 0;
    uint32_t width = 0, height = 0, duration_ms = 0;
    if (!get_video_info(video_path, video_size, width, height, duration_ms)) {
        LOGE("[video_send] 视频元数据解析失败（非 MP4 或文件不可读）: %s，本轮不发送",
             video_path.c_str());
        return false;
    }
    // PB 字段 4 单位 = 秒（捕获样例 3 = 3s 视频实证）；ms → 秒四舍五入
    const uint32_t duration_s = (duration_ms + 500) / 1000;

    // 版本横幅：部署核对就认这一行（看不到 = 跑的还是旧库）
    LOGI("[video_send] active: 视频消息(contentType=%d) path=%s size=%llu w=%u h=%u 时长=%ums→%us preview=%s 接收者=%zu",
         kVideoContentType, video_path.c_str(), static_cast<unsigned long long>(video_size),
         width, height, duration_ms, duration_s,
         preview_img_path.empty() ? "(空,交原生管线)" : preview_img_path.c_str(),
         receiver_conv_ids.size());

    // 组包落位模型层 message_pb.cpp（protobuf 生成类 sohook::VideoMessage）；
    // url/raw_url 同装本地路径（捕获样例实证 raw_url == url）；preview 空参 =
    // 字段 7 缺省不编码
    const std::vector<uint8_t> content = build_video_content_pb(
            video_path, video_size, duration_s, height, width,
            preview_img_path, video_path);

    const std::vector<uint8_t> pb = generate_wwmessage_pb(kVideoContentType, content);
    if (pb.empty()) {
        LOGE("[video_send] 出站模型 PB 序列化失败（generate_wwmessage_pb 返回空），本轮不发送");
        return false;
    }
    LOGI("[video_send] 出站模型 PB %zu 字节 (contentType=%d)，目标 %zu 个会话",
         pb.size(), kVideoContentType, receiver_conv_ids.size());

    // 群发编排（rawpb v3.1+ 同款）：detached 节流线程，每发投递主线程；
    // 发送任务全按值捕获（含整份 PB 拷贝），自包含、与编排线程无共享状态
    std::thread([pb, receiver_conv_ids, onProgress, onResult]() {
        for (size_t i = 0; i < receiver_conv_ids.size(); i++) {
            usleep(50000); // 群发节流：给企微底层 TaskQueue/网络线程让出缓冲（沿用原经验值）

            const uint64_t rid = receiver_conv_ids[i];
            MainThreadExecutor::getInstance().post(
                [pb, rid, onProgress, onResult, seq = i + 1, total = receiver_conv_ids.size()]() {
                    // 执行线程自证（真机 tid 应等于进程 pid = 主线程）
                    LOGD("[video_send] 发送任务#%zu/%zu 开始 tid=%d (conv=%llu)",
                         seq, total, current_tid(), static_cast<unsigned long long>(rid));
                    // msg_type 实为死代码（ParseFromMem 全权解析），传 5 仅为日志可读
                    send_model_message(rid, kVideoContentType, pb, onProgress, onResult);
                    LOGD("[video_send] 发送任务#%zu/%zu 已返回", seq, total);
                });
        }
        LOGI("[video_send] 群发完成: 接收者=%zu 个", receiver_conv_ids.size());
    }).detach();
    return true;
}

bool try_send_video_from_files(std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                               std::function<void(int, uint64_t)> onResult) {
    if (::access(kVideoReqPath, R_OK) != 0) {
        return false;  // video.req 不存在 = 模式未启用，调用方走原逻辑
    }

    // 输入文件存在：本次触发即由视频链路接管——即使后续失败也不回落（防同触发
    // 双发，emotion/link/rawpb 同款语义），失败原因全部写进日志，文件保留待修
    const std::string text = read_text_file(kVideoReqPath);
    ReqConfig cfg;
    if (!parse_req_config(text, "video.req", kVideoReqKeys, cfg)) {
        LOGE("[video] video.req 解析失败（%s），本次不发送", kVideoReqPath);
        return true;
    }
    std::string video_path, preview_path;
    if (!cfg.value_any({"video", "path"}, video_path)) {
        LOGE("[video] video.req 缺 video= 字段（本地 mp4 绝对路径，%s），本次不发送",
             kVideoReqPath);
        return true;
    }
    cfg.value_any({"preview", "preview_img"}, preview_path);  // 可空 = 字段 7 不编码
    const std::string &receiver_text = cfg.block("receivers");

    // 接收者：video.req 内嵌优先；未提供回退 receivers.txt（与 rawpb/emotion 共用）
    std::vector<uint64_t> receivers;
    if (!receiver_text.empty()) {
        if (!parse_receivers(receiver_text, receivers) || receivers.empty()) {
            LOGE("[video] video.req 接收者行解析失败或为空（格式同 receivers.txt："
                 "空白/逗号/分号分隔的 uint64），本次不发送");
            return true;
        }
        LOGI("[video] video.req 内嵌接收者 %zu 个", receivers.size());
    } else {
        const std::string fallback = read_text_file(kReceiversPath);
        if (fallback.empty() || !parse_receivers(fallback, receivers) || receivers.empty()) {
            LOGE("[video] 无可用接收者（video.req 未带 receivers 且 %s 无合法会话 ID），本次不发送",
                 kReceiversPath);
            return true;
        }
        LOGI("[video] video.req 未带接收者——回退 %s 命中 %zu 个", kReceiversPath, receivers.size());
    }

    // 路径早检：把"配置里写错路径"直接说成配置问题（get_video_info 也会失败，
    // 但它的报错是"非 MP4 或文件不可读"，指不到 video.req 这一行）
    if (::access(video_path.c_str(), R_OK) != 0) {
        LOGE("[video] video.req 指向的视频不可读: %s（推送视频到设备或改配置），本次不发送",
             video_path.c_str());
        return true;
    }

    // 参数直传入口（内部含 executor 主线程护栏 + 元数据解析 + 版本横幅 + 组包群发）
    if (!send_video_to_contacts(video_path, receivers, preview_path,
                                std::move(onProgress), std::move(onResult))) {
        LOGE("[video] 群发编排未受理（原因见上方 [video_send] 日志），%s 保留待修", kVideoReqPath);
        return true;
    }

    // 一次性触发：受理成功才改名，防手动连发重复群发/重复上传；失败仅告警不阻断
    if (std::rename(kVideoReqPath, kVideoSentPath) != 0) {
        LOGW("[video] %s 改名失败(%s)——下次触发会再来一轮，注意手动删除",
             kVideoReqPath, std::strerror(errno));
    }
    return true;
}
