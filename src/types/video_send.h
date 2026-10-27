//
// video_send.h — 视频文件消息发送（contentType=5，文件驱动 + 参数直传）
//
// 链路（模板 = 2026-10-22 真机捕获 275B 出站样例，nativeSetInfo 本地态）：
//   本地 mp4 → get_video_info 解析元数据（size/宽高/时长；utils/file_utils.cpp
//             moov box 级解析，宽高已按 tkhd 旋转矩阵换向）
//          → build_video_content_pb 组 VideoMessage content PB
//            （message_pb.cpp 生成类 sohook::VideoMessage，字段 1,3,4,5,6,7,202）
//          → generate_wwmessage_pb(5, content) 出站模型
//          → send_model_message 节流群发（rawpb v3.1+ 同款编排）
//
// content PB 字段配方（与捕获样例同形态）：
//   1 url=本地视频路径   3 size=物理字节   4 duration=秒（ms 四舍五入）
//   5 height  6 width（get_video_info 换向后的显示宽高）
//   7 preview_img_url=缩略图路径（空参=缺省不编码；样例的 tempimagecache
//     video_thumb .wwdata 即原生链路自生成产物——空参交原生管线生成，
//     假设待真机验证）
//   202 raw_url=与 url 同值（样例实证）
//
// 与 emotion_send 的差异：无上传/兑换半程——content 即本地态
// （url/raw_url 装本地路径），CDN 上传与缩略图生成交给原生 ChatTask 状态机
// （模型带本地路径 → 上传子树，与图片首传同模式）。
//
// 两个入口：
//   1) try_send_video_from_files —— 文件驱动（生产接线，send_hook.cpp 调用）：
//      读 <cache>/video.req（不存在 = 模式未启用，返回 false 交调用方走原逻辑），
//      受理成功后改名 video.req.sent（一次性触发，emoji.req / link.req 同款语义）。
//   2) send_video_to_contacts   —— 参数直传（供上层/调试直接指定路径与对象）。
//
// video.req 格式（与 link.req 同一文法，config/req_config.cpp 承载：'#' 注释、
// 空行忽略、key=value 只切第一个 '='、未知字段整体失败并报行号）：
//   video=       本地 mp4 绝对路径（必填；别名 path=）
//   preview=     本地缩略图路径（可空/缺省 = 字段 7 不编码，生成交原生管线；
//                别名 preview_img=）
//   receivers=   接收者会话 ID，可多行累加，格式同 receivers.txt；整段缺省则
//                回退 <cache>/receivers.txt
// 样例：仓库 file/video.req；推送：adb push file/video.req <cache>/video.req。
//
// 线程模型：入口本地文件读 + 组包（无原生服务调用）+ executor 主线程护栏
// （首调拉起 MainThreadExecutor——参数直传入口无前段顺路初始化；executor
// 就绪后任意线程可调，首调须在主线程）；群发 = detached 节流线程，每次
// send_model_message 经 MainThreadExecutor 投递主线程
// 执行（发送原语主线程亲和）。入口无共享状态、无轮询。
//
// 落位：src/types/video_send.{h,cpp}；
//
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 文件驱动入口：video.req 不存在返回 false（未启用，调用方走原逻辑）；存在则
// 本次触发由视频链路接管，即使解析/发送失败也返回 true（防同触发双发），
// 失败原因全部写进日志且文件保留待修。
// 回调签名与 send_hook.cpp 的 on_progress/on_result 一致（link_send 同款直写）：
//   onProgress(current, total, msg_local_id)   onResult(errcode, msg_local_id)
bool try_send_video_from_files(std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                               std::function<void(int, uint64_t)> onResult);

// 发送视频文件消息到多个会话（参数直传；立即返回，群发异步）：
//   video_path         本地 mp4 绝对路径（企微私有目录如
//                      /storage/emulated/0/Android/data/com.tencent.wework/files/
//                      DCIM/temp/tempVideo/CustomCameraVideo.mp4，sohook 同进程直读）
//   receiver_conv_ids  接收会话 ID 列表
//   preview_img_path   本地缩略图路径，可空（空 = 字段 7 缺省不编码，缩略图生成
//                      交原生管线）
//   返回 true = 已受理（元数据解析成功、PB 就绪、群发已编排）；false = 校验失败
//   （路径/接收者空、元数据解析失败、序列化失败），失败原因见日志
bool send_video_to_contacts(const std::string &video_path,
                            const std::vector<uint64_t> &receiver_conv_ids,
                            const std::string &preview_img_path = "",
                            std::function<void(int64_t, int64_t, uint64_t)> onProgress = {},
                            std::function<void(int, uint64_t)> onResult = {});
