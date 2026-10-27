//
// emotion_send.h — 图片转表情消息发送（contentType=29 全链路）
//
// 链路（复刻 App 自定义表情发送侧形态，模板 = 2026-10-16 真机捕获 452 字节实包）：
//   本地图片 → 独立 CDN 上传（gif_url_exchange 全量变体）拿 (fileId/aesKey/md5)
//            → 服务端换 URL 拿 (s1 主图 / s2 封面 / s3 md5 回显)
//            → 组 EmotionMessage content PB（message_pb.cpp 生成类组包，字段见
//              proto/ww_emotion_message.proto）
//            → generate_wwmessage_pb(29, content) 出站模型
//            → send_model_message 节流群发（rawpb v3.1+ 同款编排）
//
// content PB 字段配方（捕获实包逐字节对齐，字段升序）：
//   1 url=s1  3 file_id=上传票据  4 size=本地文件字节  5 aes_key  6 md5
//   7 width  8 height  9 description="自定义表情"  11 source_type=2(USER_ADD)
//   12 static_url=s2；type(2)/group_id(10)/create_time(13)/src(14)… 缺省不编码
//
// 线程模型：入口在 my_nativeSend（主线程）装载文件；上传/兑换经 gif_url_exchange
// 纯回调链；全量回调（框架 CGI 线程）组包后 detached 节流线程群发，每次
// send_model_message 投递主线程执行（rawpb v3.1+ 同款）。
//
// 落位：src/types/emotion_send.{h,cpp}；接线见本补丁目录 README §8。
//
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// content PB 组包 build_emotion_content_pb(...) 落位 message/message_pb.cpp（模型层，
// protobuf 生成类 sohook::EmotionMessage 构造，字段配方见上）；本模块 include
// "message/message_pb.h" 后只做编排引用——与 generate_wwmessage_pb(29, content) 同层。

// 群发回调类型（与 raw_pb_broadcast / send_model_message 一致）
using EmotionProgress = std::function<void(int64_t, int64_t, uint64_t)>;
using EmotionResult = std::function<void(int, uint64_t)>;

// 发送汇点（设备链路 = 全量回调触达；公开仅供宿主单测注入票据直测组包/群发）：
// 校验七元组 → 组 content PB → generate_wwmessage_pb(29,·) → 节流群发。
// errcode：0=成功；-1=上传半程失败（合成）；>0=兑换 CGI 错误码。
bool emotion_finish_and_send(int errcode, const std::string &file_id, const std::string &aes_key,
                             const std::string &md5,
                             const std::string &s1, const std::string &s2, const std::string &s3,
                             uint64_t file_size, uint32_t width, uint32_t height,
                             const std::vector<uint64_t> &receivers,
                             EmotionProgress onProgress = {}, EmotionResult onResult = {});

// 文件驱动一次性入口（my_nativeSend 主线程触发，返回 true = 本次触发已由本模块接管）：
//   <cache>/emoji.req —— 首个非注释行 = 本地图片绝对路径（缺省 /storage/emulated/0/test.jpg）；
//                        其余非注释内容 = 接收会话 ID（'#' 注释 + 空白/逗号/分号分隔的
//                        uint64，与 receivers.txt 同格式）；未提供时回退 <cache>/receivers.txt
//   装载成功即改名 emoji.req.sent（防手动连发重复上传/群发）；上传参数固定 App 表情流
//   实测 (1,42,-1)（ylj.d0 → CdnUploadFileOnPath(path,1,cb,prog,42)）
bool try_send_emotion_from_files(EmotionProgress onProgress = {}, EmotionResult onResult = {});
