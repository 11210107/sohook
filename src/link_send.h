//
// link_send.h — 链接卡片消息发送（contentType=13，文件驱动 + 参数直传）
//
// 链路（content_type=13 = HyperLinkMessage 三重实证：sub_FCFC84 switch
// case 13 日志串 + 视频号分享卡实机样例 38 0d 内层 {1,2,3,4} 同构 + frida
// LinkMessageProto）：
//   url/image_url/title/description 参数
//        → build_link_content_pb 组 LinkMessage content PB
//          （message_pb.cpp 生成类 sohook::LinkMessage，字段 1,2,3,4 条件 set）
//        → generate_wwmessage_pb(13, content) 出站模型
//        → send_model_message 节流群发（rawpb v3.1+ 同款编排）
//
// content PB 字段配方（最小集——视频号实机样例 {1,2,3,4} 即完整卡片）：
//   1 link_url=链接地址   2 image_url=封面图 URL（远程，接收端自行下载；
//   空参=缺省不编码）   3 title=标题   4 description=描述（可空）
//
// 与 video_send 的差异：无媒体文件 IO（只读 link.req 文本配置）、无元数据解析、
// 无上传半程——纯 URL 组包
// 即发（content 即完整态），任意线程可调。CDN/本地字段（image_data/
// open_image_uri 等）与来源元数据（appid_type/appname 等）均不 set——
// 最小集出站，扩展字段待实机样例再议。
//
// 线程模型：入口纯内存组包（无原生服务调用）+ executor 主线程护栏（首调拉起
// MainThreadExecutor——参数直传入口无前段顺路初始化；executor 就绪后任意
// 线程可调，首调须在主线程）；群发 = detached 节流线程，每次
// send_model_message 经 MainThreadExecutor 投递主线程执行（发送原语主线程
// 亲和）。无共享状态。onProgress 仅在原生协议层有进度回调时触发（透传）。
//
// 两个入口：
//   1) try_send_link_from_files —— 文件驱动（生产接线，send_hook.cpp 调用）：
//      读 <cache>/link.req（不存在 = 模式未启用，返回 false 交调用方走原逻辑），
//      key=value 格式见下方说明；受理成功后改名 link.req.sent（一次性触发，
//      emoji.req / pb.hex 同款语义）。
//   2) send_link_to_contacts   —— 参数直传（供上层/调试直接指定内容）。
//
// link.req 格式（'#' 注释、空行忽略；key=value 只切第一个 '='，URL 内 '=' 安全；
// 未知 key 整体失败）：
//   url=          链接地址（必填）
//   image_url=    封面图 URL（空/缺省 = 字段 2 不编码）
//   title=        卡片标题（空/缺省 = 字段 3 不编码）
//   description=  卡片描述（空/缺省 = 字段 4 不编码；别名 desc=）
//   receivers=    接收者会话 ID，可多行累加，格式同 receivers.txt；整段缺省则
//                 回退 <cache>/receivers.txt
// 样例：仓库 file/link.req；推送：adb push file/link.req <cache>/link.req。
//
// 落位：src/types/link_send.{h,cpp}；
//
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 文件驱动入口：link.req 不存在返回 false（未启用，调用方走原逻辑）；存在则
// 本次触发由链接链路接管，即使解析/发送失败也返回 true（防同触发双发），
// 失败原因全部写进日志且文件保留待修。
bool try_send_link_from_files(std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                              std::function<void(int, uint64_t)> onResult);

// 发送链接卡片消息到多个会话（参数直传；立即返回，群发异步）：
//   url                链接地址（如 https://mp.weixin.qq.com/s?...）
//   image_url          封面图 URL（如 https://wework.qpic.cn/wwpic3az/...；
//                      空 = 字段 2 缺省不编码，卡片无封面图）
//   title              卡片标题（如文章标题；空 = 字段 3 缺省不编码）
//   receiver_conv_ids  接收会话 ID 列表
//   description        卡片描述（可空 = 字段 4 缺省不编码）
//   onProgress         进度回调（链接无上传半程，仅原生协议层触发时回调；可传空）
//   onResult           结果回调（errcode + conv；可传空）
//   返回 true = 已受理（PB 就绪、群发已编排）；false = 校验失败（url/接收者
//   空、executor 不可用、序列化失败），失败原因见日志
bool send_link_to_contacts(const std::string &url,
                           const std::string &image_url,
                           const std::string &title,
                           const std::vector<uint64_t> &receiver_conv_ids,
                           const std::string &description,
                           std::function<void(int64_t, int64_t, uint64_t)> onProgress,
                            std::function<void(int, uint64_t)> onResult);
