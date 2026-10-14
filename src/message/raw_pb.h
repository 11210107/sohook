//
// raw_pb.h — 直接 PB 流发送：hex 解析 + 出站清洗（wire 级字段过滤）
//
// 用途：把拦截捕获的完整 Message PB（framework.js nativeSetInfo 完整打印的
// hex 流，如视频号/小程序消息）直接作为发送模型重放——与 generate_*_pb 用
// protobuf 类逐字段拼装并列的另一种发消息形式。
//
// 为什么要清洗：捕获流里混有原消息的身份/路由/状态字段（id、remote_id、
// conversation_id、seq、sender、send_time、state、progress…），直接出站等于
// 带着【别人的消息身份】投递——落库按模型字段写本地记录（sendModelMsg.md §4），
// 陈旧 id/seq 轻则渲染错乱，重则与服务端现有消息冲突。故发送前做 wire 级
// 清洗：保留"消息是什么"的内容字段，剥掉"消息从哪来"的身份字段，与
// generate_*_pb 只填 Tag7+Tag10 的出站哲学一致（README §11：PB content_type
// 是 Message 类型的唯一来源）。
//
// 落位：src/message/raw_pb.{h,cpp}
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rawpb {

// hex 文本 → PB 字节流。
// 容忍：多行、任意空白/逗号/分号分隔、'#' 起始的注释、"0x" 前缀、无分隔连续 hex。
// 拒绝：奇数个十六进制字符（半字节——捕获流被截断的典型特征）、非法字符。
// 成功返回 true 并填充 out；失败返回 false（日志指明行号），out 清空。
bool parse_hex_stream(const std::string &hex_text, std::vector<uint8_t> &out);

// 出站清洗：wire 级扫描，白名单字段原样保留（嵌套 payload 逐字节不动），
// 其余字段剔除。顺带完成 wire 完整性校验——长度越界（捕获流被截断）时返回
// 空 vector 并打 ERROR，绝不把残缺 PB 送进发送管线。
// stripped_fields（可选）回填被剔除的字段个数，供日志自证。
std::vector<uint8_t> sanitize_message_pb_for_send(const std::vector<uint8_t> &raw,
                                                  size_t *stripped_fields = nullptr);

// 仅校验 wire 完整性（不改动字段）。#keep-all 原样重放模式用。
bool validate_message_pb(const std::vector<uint8_t> &raw);

// 提取外层 content_type（Tag 7 varint），取不到返回 -1。仅日志用途，
// 尽力而为：Tag 7 之后的 wire 截断不影响取值（截断流也能报出类型），
// 严格完整性校验由 validate/sanitize 负责。create_message_pure_native_ptr
// 的 type 参数是死代码（README §11），真正进状态机的类型来自 PB 的 Tag 7。
int extract_content_type(const std::vector<uint8_t> &pb);

} // namespace rawpb
