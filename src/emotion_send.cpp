//
// emotion_send.cpp — 图片转表情消息发送（contentType=29 全链路）
//
// 证据链：
//  * contentType=29 = 表情消息（用户真机实测指示；捕获实包即该类型出站）
//  * content PB 模板 = 2026-10-16 真机捕获 452 字节实包（message_pb.cpp 组包器与其
//    逐字节一致；宿主单测 emotion_test.cpp 内嵌该 hex 做字节级回归）。字段实测值：
//      1  url        https://wework.qpic.cn/wwpic3az/326415_T4j1lR7gRQaUmlc_1792034342/0 (=s1)
//      3  file_id    3069…0400（214 字符上传票据）
//      4  size       532504（本地文件字节）
//      5  aes_key    195f75aea5e340b08af30f45fc5c097a
//      6  md5        a9b410dfd3ac9d01c68418df436da4df
//      7  width 300  8  height 288
//      9  description 自定义表情（UTF-8 15 字节）
//      11 source_type 2 (USER_ADD)
//      12 static_url https://wework.qpic.cn/wwpic3az/693481_tBOU2qQkRISiASP_1792034341/0 (=s2)
//    字段 2 type 等 缺省不编码（捕获即如此——即使实发 GIF 动图也省，保持逐字节一致）。
//  * 出站/群发 = generate_wwmessage_pb(29, content) + send_model_message（rawpb
//    v3.1+ 编排：detached 节流线程 + 每发主线程投递；msg_type 实为死代码——模型 PB
//    经 ParseFromMem 全权解析，传 29 仅为日志可读，与 raw_pb_broadcast 同结论）。
//  * 图片头解析：PNG(IHDR BE)/JPEG(SOF 段扫描)/GIF(LSD LE)/BMP——工程
//    utils/file_utils.cpp parse_image_info（commit 291942d 并入，含 GIF），经
//    #include "utils/file_utils.h" 引用；本模块原自带拷贝已删（与 parse_receivers
//    同理收敛，且消除与工程全局声明的重载歧义）。
//
// 线程模型：入口主线程装载 + 改名；原生半程经 gif_url_exchange 全量变体（纯回调、
// 主线程投递）；全量回调（框架 CGI 线程）只做组包 + detached 节流线程群发，每次
// send_model_message 投递主线程——无共享状态、无轮询。
//
// 落位：src/emotion_send.{h,cpp}；接线见本补丁目录 README §8。
//
#include "emotion_send.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

#include "cdn_url_exchange.h"
#include "logger.h"
#include "main_thread_executor.h"
#include "message_sender.h"
#include "message/message_pb.h"
#include "utils/file_utils.h"
#include "utils/receiver_utils.h"

// 配置目录与 gif_url_exchange 共用（可被宿主单测覆写；Android 构建走默认值）
#ifndef GIF_URL_CONFIG_DIR
#define GIF_URL_CONFIG_DIR "/storage/emulated/0/Android/data/com.tencent.wework/cache"
#endif

namespace {

constexpr const char *kEmojiReqPath  = GIF_URL_CONFIG_DIR "/emoji.req";
constexpr const char *kEmojiSentPath = GIF_URL_CONFIG_DIR "/emoji.req.sent";
constexpr const char *kReceiversPath = GIF_URL_CONFIG_DIR "/receivers.txt";  // emoji.req 未带 ID 时回退（与 rawpb 共用）
constexpr const char *kDefaultImagePath = "/storage/emulated/0/test.jpg";

// "自定义表情"（UTF-8 15 字节，App 收集流 mDesc 实测值；转义写法防源码编码漂移）
constexpr const char *kDefaultDescription =
        "\xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89\xe8\xa1\xa8\xe6\x83\x85";
constexpr uint32_t kSourceTypeUserAdd = 2;   // EmotionSourceType.USER_ADD

constexpr int kEmotionContentType = 29;      // 表情消息 contentType（真机实测）
constexpr int kUploadScene = 1;              // App 表情流上传参数实测 (1,42,-1)
constexpr int kUploadSubScene = 42;
constexpr int kUploadType = -1;

// 当前线程 ID：Android/Linux 用 SYS_gettid；宿主单测(macOS) 用 thread_selfid
// （gif_url_exchange.cpp 同款拷贝，本模块不依赖其内部符号）
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

} // namespace

bool emotion_finish_and_send(int errcode, const std::string &file_id, const std::string &aes_key,
                             const std::string &md5,
                             const std::string &s1, const std::string &s2, const std::string &s3,
                             uint64_t file_size, uint32_t width, uint32_t height,
                             const std::vector<uint64_t> &receivers,
                             EmotionProgress onProgress, EmotionResult onResult) {
    if (errcode != 0) {
        LOGE("[emotion] 链路失败 errcode=%d（-1=上传半程合成 / >0=兑换 CGI 错误码），本轮不发送",
             errcode);
        return false;
    }
    if (file_id.empty() || aes_key.empty() || md5.empty() ||
        s1.empty() || s2.empty() || s3.empty()) {
        LOGE("[emotion] 七元组有空串: fileid=%zuB aes=%zuB md5=%zuB s1=%zuB s2=%zuB s3=%zuB → 本轮不发送",
             file_id.size(), aes_key.size(), md5.size(), s1.size(), s2.size(), s3.size());
        return false;
    }
    if (receivers.empty()) {
        LOGE("[emotion] 接收者列表为空，本轮不发送");
        return false;
    }

    LOGI("[emotion] ★ 全量票据就绪: fileid=%.32s... size=%llu w=%u h=%u s1=[%.48s...] s2=[%.48s...]",
         file_id.c_str(), static_cast<unsigned long long>(file_size), width, height,
         s1.c_str(), s2.c_str());

    // 组包落位模型层 message_pb.cpp（protobuf 生成类 sohook::EmotionMessage；
    // description/source_type 固定值与捕获实包一致："自定义表情" / 2=USER_ADD）
    const std::vector<uint8_t> content = build_emotion_content_pb(
            s1, file_id, file_size, aes_key, md5, width, height,
            kDefaultDescription, kSourceTypeUserAdd, s2);
    LOGI("[emotion] content PB 组包完成: %zu 字节（字段 1,3,4,5,6,7,8,9,11,12 升序，缺省不编码）",
         content.size());

    const std::vector<uint8_t> pb = generate_wwmessage_pb(kEmotionContentType, content);
    if (pb.empty()) {
        LOGE("[emotion] 出站模型 PB 序列化失败（generate_wwmessage_pb 返回空），本轮不发送");
        return false;
    }
    LOGI("[emotion] 出站模型 PB %zu 字节 (contentType=%d)，目标 %zu 个会话",
         pb.size(), kEmotionContentType, receivers.size());

    // 群发编排（rawpb v3.1+ 同款）：detached 节流线程，每发投递主线程；
    // 发送任务全按值捕获（含整份 PB 拷贝），自包含、与编排线程无共享状态
    std::thread([pb, receivers, onProgress, onResult]() {
        for (size_t i = 0; i < receivers.size(); i++) {
            usleep(50000); // 群发节流：给企微底层 TaskQueue/网络线程让出缓冲（沿用原经验值）

            const uint64_t rid = receivers[i];
            MainThreadExecutor::getInstance().post(
                [pb, rid, onProgress, onResult, seq = i + 1, total = receivers.size()]() {
                    // 执行线程自证（tid 应等于进程 pid = 主线程）
                    LOGD("[emotion] 发送任务#%zu/%zu 开始 tid=%d (conv=%llu)",
                         seq, total, current_tid(), static_cast<unsigned long long>(rid));
                    // msg_type 实为死代码（ParseFromMem 全权解析），传 29 仅为日志可读
                    send_model_message(rid, kEmotionContentType, pb, onProgress, onResult);
                    LOGD("[emotion] 发送任务#%zu/%zu 已返回", seq, total);
                });
        }
        LOGI("[emotion] 群发完成: 接收者=%zu 个", receivers.size());
    }).detach();
    return true;
}

bool try_send_emotion_from_files(EmotionProgress onProgress, EmotionResult onResult) {
    if (::access(kEmojiReqPath, R_OK) != 0) {
        return false;  // emoji.req 不存在 = 模式未启用，调用方走原逻辑
    }

    // 输入文件存在：本次触发即由表情链路接管——即使后续失败也不回落（防同触发双发，
    // rawpb 同款语义），失败原因全部写进日志
    const std::string text = read_text_file(kEmojiReqPath);

    // 解析：'#' 注释；首个非注释行 = 图片路径（缺省 kDefaultImagePath）；
    // 其余非注释行合并 = 接收会话 ID（空白/逗号/分号分隔）
    std::string image_path;
    std::string receiver_text;
    bool have_path = false;
    auto trim = [](std::string &s) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        const size_t e = s.find_last_not_of(" \t\r\n");
        s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    };
    {
        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
            pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
            trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (!have_path) {
                image_path = line;
                have_path = true;
                continue;
            }
            receiver_text += line;
            receiver_text += '\n';
        }
    }
    if (!have_path || image_path.empty()) {
        image_path = kDefaultImagePath;
        LOGI("[emotion] emoji.req 无路径行——使用缺省图片 %s", kDefaultImagePath);
    }

    // 接收者：emoji.req 内嵌优先；未提供回退 receivers.txt（与 rawpb 共用同一文件）
    std::vector<uint64_t> receivers;
    if (!receiver_text.empty()) {
        if (!parse_receivers(receiver_text, receivers) || receivers.empty()) {
            LOGE("[emotion] emoji.req 接收者行解析失败或为空（格式同 receivers.txt：空白/逗号/分号分隔的 uint64），本次不发送");
            return true;
        }
        LOGI("[emotion] emoji.req 内嵌接收者 %zu 个", receivers.size());
    } else {
        const std::string fallback = read_text_file(kReceiversPath);
        if (fallback.empty() || !parse_receivers(fallback, receivers) || receivers.empty()) {
            LOGE("[emotion] 无可用接收者（emoji.req 未带 ID 且 %s 无合法会话 ID），本次不发送",
                 kReceiversPath);
            return true;
        }
        LOGI("[emotion] emoji.req 未带接收者——回退 %s 命中 %zu 个", kReceiversPath, receivers.size());
    }
    /*
     */
    // 图片信息（尺寸进 content PB；解析用工程 utils/file_utils.cpp parse_image_info，含 GIF）
    const std::vector<uint8_t> image_data = read_binary_file(image_path);
    if (image_data.empty()) {
        LOGE("[emotion] 图片读取失败或为空: %s，本次不发送", image_path.c_str());
        return true;
    }
    uint32_t width = 0, height = 0;
    if (!parse_image_info(image_data, width, height)) {
        LOGE("[emotion] 图片头解析失败（支持 PNG/JPEG/GIF/BMP）: %s，本次不发送", image_path.c_str());
        return true;
    }
    const uint64_t file_size = image_data.size();

    // 主线程护栏：上传/兑换编排要求主线程（executor 绑定当前线程 Looper；
    // my_nativeSend 实测主线程）。放行前不改名——非主线程触发不消耗一次性文件
    if (!is_main_thread()) {
        LOGE("[emotion] 调用线程不是主线程(tid=%d, pid=%d)，中止", current_tid(), getpid());
        return true;
    }

    // 版本横幅：部署核对就认这一行（看不到 = 跑的还是旧库）
    LOGI("[emotion] active: 图片→表情(contentType=%d), path=%s size=%llu w=%u h=%u 接收者=%zu",
         kEmotionContentType, image_path.c_str(), static_cast<unsigned long long>(file_size),
         width, height, receivers.size());

    // 一次性触发：装载成功即改名，防手动连发重复上传/群发；失败仅告警不阻断
    if (std::rename(kEmojiReqPath, kEmojiSentPath) != 0) {
        LOGW("[emotion] %s 改名失败(%s)——下次触发会再来一轮，注意手动删除",
             kEmojiReqPath, std::strerror(errno));
    }

    // 全量回调（框架 CGI 线程触达）：补图片尺寸/接收者/业务回调 → 汇点组包群发
    GifUrlFullResult full = [file_size, width, height, receivers, onProgress, onResult](
            int errcode, const std::string &file_id, const std::string &aes_key,
            const std::string &md5, const std::string &s1, const std::string &s2,
            const std::string &s3) {
        emotion_finish_and_send(errcode, file_id, aes_key, md5, s1, s2, s3,
                                file_size, width, height, receivers, onProgress, onResult);
    };
    upload_and_exchange_gif_url_full(image_path, kUploadScene, kUploadSubScene, kUploadType,
                                     std::move(full));

    /*
    upload_and_exchange_gif_url_full(image_path, kUploadScene, kUploadSubScene, kUploadType,
                                     [](int errcode, const std::string &file_id,
                                        const std::string &aes_key,
                                        const std::string &md5, const std::string &s1,
                                        const std::string &s2,
                                        const std::string &s3) {
                                         if (errcode != 0) {
                                             LOGE("[emotion] 上传/兑换失败 errcode=%d", errcode);
                                             return;
                                         }
                                         if (file_id.empty() || aes_key.empty() || md5.empty() ||
                                             s1.empty() || s2.empty() || s3.empty()) {
                                             LOGE("[emotion] 七元组有空串: fileid=%zuB aes=%zuB md5=%zuB s1=%zuB s2=%zuB s3=%zuB",
                                                  file_id.size(), aes_key.size(), md5.size(),
                                                  s1.size(), s2.size(), s3.size());
                                             return;
                                         }
                                         // content 与接收者无关：同一份字节可供多个对象经 msg_send_pb 复用发送
                                         LOGD("[emotion] 上传/兑换成功: fileid=%s aes=%s md5=%s s1=%s s2=%s s3=%s",
                                              file_id.c_str(), aes_key.c_str(), md5.c_str(),
                                              s1.c_str(), s2.c_str(), s3.c_str());
                                     });
    */
    return true;
}
