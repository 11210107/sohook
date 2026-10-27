//
// link_send.cpp — 链接卡片消息发送（contentType=13，参数直传版）
//
// 证据链：
//  * contentType=13 = HyperLinkMessage（sub_FCFC84 switch case 13 →
//    "发送了一条超链接消息：[HyperLinkMessage]" 日志串实证；视频号分享卡
//    实机样例外层 38 0d，内层 f1/f2/f3/f4 = link_url/image_url/title/
//    description 与 LinkMessage 逐字段同构——13 即通用链接卡片，视频号是
//    其一种实例）
//  * content PB 模板 = 视频号样例最小集 {1,2,3,4}（纯 URL 三元组 + 描述，
//    无文件 IO、无 CDN 上传——封面 image_url 为远程 URL，接收端自行下载）
//  * 组包 = message/message_pb.cpp build_link_content_pb（生成类
//    sohook::LinkMessage，字段 1,2,3,4 升序、空串条件 set）
//
// 线程模型：入口纯内存组包（无原生服务调用）+ executor 主线程护栏（首调拉起
// MainThreadExecutor——参数直传入口无 gif_url_exchange 前段顺路初始化；
// 2026-10-22 真机实证无护栏时 post 静默丢弃、群发零投递）；executor 就绪后
// 任意线程可调。群发 = detached 节流线程，每发经 MainThreadExecutor 投递
// 主线程（发送原语主线程亲和）——rawpb v3.1+ / video_send 同款编排，无共享
// 状态。
//
// 落位：src/types/link_send.{h,cpp}；
//
#include "link_send.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "logger.h"
#include "main_thread_executor.h"
#include "message_sender.h"
#include "message/message_pb.h"
#include "utils/file_utils.h"
#include "utils/address_utils.h"
#include "utils/receiver_utils.h"
#include "utils/req_config.h"

// 配置目录与 emoji.req / pb.hex 同一处（可被宿主单测覆写；Android 构建走默认值）
#ifndef LINK_CONFIG_DIR
#define LINK_CONFIG_DIR "/storage/emulated/0/Android/data/com.tencent.wework/cache"
#endif

namespace {

constexpr int kLinkContentType = 13;  // HyperLinkMessage（sub_FCFC84 case 13
                                      // 日志串 + 视频号样例 38 0d 实证）

constexpr const char *kLinkReqPath   = LINK_CONFIG_DIR "/link.req";
constexpr const char *kLinkSentPath  = LINK_CONFIG_DIR "/link.req.sent";
constexpr const char *kReceiversPath = LINK_CONFIG_DIR "/receivers.txt";  // link.req 未带 ID 时回退（与 rawpb/emotion 共用）

// 当前线程 ID：Android/Linux 用 SYS_gettid；宿主单测(macOS) 用 thread_selfid
// （video_send.cpp 同款拷贝，本模块不依赖其内部符号）
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

// link.req 合法字段（小写；desc 是 description 的别名）——文法与严格性由
// utils/req_utils.cpp 统一承载（video.req 共用同一实现）
const std::vector<std::string> kLinkReqKeys = {
        "url", "image_url", "title", "description", "desc", "receivers"};

} // namespace

bool send_link_to_contacts(const std::string &url,
                           const std::string &image_url,
                           const std::string &title,
                           const std::vector<uint64_t> &receiver_conv_ids,
                           const std::string &description,
                           std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                            std::function<void(int, uint64_t)> onResult) {
    if (url.empty()) {
        LOGE("[link_send] 链接地址为空，本轮不发送");
        return false;
    }
    if (receiver_conv_ids.empty()) {
        LOGE("[link_send] 接收者列表为空，本轮不发送");
        return false;
    }
    // 主线程护栏：MainThreadExecutor::initialize() 绑定【当前线程】的 Looper，
    // 必须由主线程调（image_broadcast v3.1-3 / raw_pb_broadcast 同款判据）。
    // 参数直传入口不经 gif_url_exchange 前段，executor 不会"顺路"被初始化——
    // 首调必须自己拉起（2026-10-22 真机实证：无护栏时 post 静默丢弃，
    // "群发完成"横幅照打但零投递，发送任务 lambda 从未执行）
    MainThreadExecutor &executor = MainThreadExecutor::getInstance();
    if (!executor.isInitialized()) {
        if (!is_main_thread()) {
            LOGE("[link_send] executor 未初始化且调用线程非主线程(tid=%d)，"
                 "无法在此初始化（Looper 将绑到错误线程），本轮不发送", current_tid());
            return false;
        }
        if (!executor.initialize()) {
            LOGE("[link_send] executor 初始化失败，本轮不发送");
            return false;
        }
        LOGI("[link_send] MainThreadExecutor 初始化完成（主线程护栏首调）");
    }
    // 版本横幅：部署核对就认这一行（看不到 = 跑的还是旧库）
    LOGI("[link_send] active: 链接消息(contentType=%d) url=%s 封面=%s 标题=%s 描述=%s 接收者=%zu",
         kLinkContentType, url.c_str(),
         image_url.empty() ? "(空)" : image_url.c_str(),
         title.empty() ? "(空)" : title.c_str(),
         description.empty() ? "(空)" : description.c_str(),
         receiver_conv_ids.size());

    // 组包落位模型层 message_pb.cpp（protobuf 生成类 sohook::LinkMessage）；
    // 空串参数 = 对应字段缺省不编码（条件 set）
    const std::vector<uint8_t> content = build_link_content_pb(
            url, image_url, title, description);

    const std::vector<uint8_t> pb = generate_wwmessage_pb(kLinkContentType, content);
    if (pb.empty()) {
        LOGE("[link_send] 出站模型 PB 序列化失败（generate_wwmessage_pb 返回空），本轮不发送");
        return false;
    }
    LOGI("[link_send] 出站模型 PB %zu 字节 (contentType=%d)，目标 %zu 个会话",
         pb.size(), kLinkContentType, receiver_conv_ids.size());

    // 群发编排（rawpb v3.1+ 同款）：detached 节流线程，每发投递主线程；
    // 发送任务全按值捕获（含整份 PB 拷贝），自包含、与编排线程无共享状态
    std::thread([pb, receiver_conv_ids,onProgress, onResult]() {
        for (size_t i = 0; i < receiver_conv_ids.size(); i++) {
            usleep(50000); // 群发节流：给企微底层 TaskQueue/网络线程让出缓冲（沿用原经验值）

            const uint64_t rid = receiver_conv_ids[i];
            MainThreadExecutor::getInstance().post(
                [pb, rid,onProgress, onResult, seq = i + 1, total = receiver_conv_ids.size()]() {
                    // 执行线程自证（真机 tid 应等于进程 pid = 主线程）
                    LOGD("[link_send] 发送任务#%zu/%zu 开始 tid=%d (conv=%llu)",
                         seq, total, current_tid(), static_cast<unsigned long long>(rid));
                    // msg_type 实为死代码（ParseFromMem 全权解析），传 13 仅为日志可读；
                    // onProgress 透传（链接无上传半程，仅原生协议层触发时回调）
                    send_model_message(rid, kLinkContentType, pb, onProgress, onResult);
                    LOGD("[link_send] 发送任务#%zu/%zu 已返回", seq, total);
                });
        }
        LOGI("[link_send] 群发完成: 接收者=%zu 个", receiver_conv_ids.size());
    }).detach();
    return true;
}

bool try_send_link_from_files(std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                              std::function<void(int, uint64_t)> onResult) {
    if (::access(kLinkReqPath, R_OK) != 0) {
        return false;  // link.req 不存在 = 模式未启用，调用方走原逻辑
    }

    // 输入文件存在：本次触发即由链接链路接管——即使后续失败也不回落（防同触发
    // 双发，emotion/rawpb 同款语义），失败原因全部写进日志，文件保留待修
    const std::string text = read_text_file(kLinkReqPath);
    ReqConfig cfg;
    if (!parse_req_config(text, "link.req", kLinkReqKeys, cfg)) {
        LOGE("[link] link.req 解析失败（%s），本次不发送", kLinkReqPath);
        return true;
    }
    std::string url, image_url, title, description;
    cfg.value("url", url);
    cfg.value("image_url", image_url);
    cfg.value("title", title);
    cfg.value_any({"description", "desc"}, description);
    const std::string &receiver_text = cfg.block("receivers");

    if (url.empty()) {
        LOGE("[link] link.req 缺 url= 字段（%s），本次不发送", kLinkReqPath);
        return true;
    }

    // 接收者：link.req 内嵌优先；未提供回退 receivers.txt（与 rawpb/emotion 共用）
    std::vector<uint64_t> receivers;
    if (!receiver_text.empty()) {
        if (!parse_receivers(receiver_text, receivers) || receivers.empty()) {
            LOGE("[link] link.req 接收者行解析失败或为空（格式同 receivers.txt："
                 "空白/逗号/分号分隔的 uint64），本次不发送");
            return true;
        }
        LOGI("[link] link.req 内嵌接收者 %zu 个", receivers.size());
    } else {
        const std::string fallback = read_text_file(kReceiversPath);
        if (fallback.empty() || !parse_receivers(fallback, receivers) || receivers.empty()) {
            LOGE("[link] 无可用接收者（link.req 未带 receivers 且 %s 无合法会话 ID），本次不发送",
                 kReceiversPath);
            return true;
        }
        LOGI("[link] link.req 未带接收者——回退 %s 命中 %zu 个", kReceiversPath, receivers.size());
    }

    // 参数直传入口（内部含 executor 主线程护栏 + 版本横幅 + 组包群发）
    if (!send_link_to_contacts(url, image_url, title, receivers, description,
                              std::move(onProgress), std::move(onResult))) {
        LOGE("[link] 群发编排未受理（原因见上方 [link_send] 日志），%s 保留待修", kLinkReqPath);
        return true;
    }

    // 一次性触发：受理成功才改名，防手动连发重复群发；失败仅告警不阻断
    if (std::rename(kLinkReqPath, kLinkSentPath) != 0) {
        LOGW("[link] %s 改名失败(%s)——下次触发会再来一轮，注意手动删除",
             kLinkReqPath, std::strerror(errno));
    }
    return true;
}
