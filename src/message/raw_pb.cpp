//
// raw_pb.cpp — 直接 PB 流发送：hex 解析 + 出站清洗（wire 级字段过滤）
//
// 实测样例（2026-10-12，framework.js 完整打印的 nativeSetInfo 拦截流）：
//   视频号 content_type=13（trace/pb_video_channel.hex，1973 字节，已逐字段验证）：
//     Message{ id(1) | content_type(7)=13
//              | content(10){1:页面url(带token) 2:stodownload媒体url 3:标题
//                4:来源名 153:feed{1:类型=4 2:媒体url(同content.2) 4:头像url
//                5:昵称 6:摘要 7/15:页面url 17:1}}
//              | extra_content(19){3045:{1:32B key 2:13(=content_type)
//                3:{1:发送时间(s) 3:base64 token 6:视频发布时间(µs) ...}}} }
//   小程序 content_type=78（trace/pb_miniprogram.hex，1996 字节，粘贴被截断）：
//     Message{ id(1) | content_type(7)=78 | content(10){107:{1:gh账号
//              2:appid 3:页面路径…}} }
//   两者内容均自包含（URL/文本，无本地文件）→ 重放不触发上传分支。
//
#include "message/raw_pb.h"
#include "utils/logger.h"

namespace rawpb {

namespace {

// 出站白名单：只保留"消息是什么"的内容字段。以下全部剔除（字段号语义见
// proto/wwmessage.proto）：
//   1 id / 2 remote_id / 4 conversation_id / 5 seq / 6 sender —— 原消息身份与路由
//   3 conv_type —— 出站由目标会话句柄决定（send_model_message 的 conv 容器）
//   8 send_time / 12 flag / 14 state / 16 ack_send_state / 17 devinfo —— 原消息状态
//   15 extras —— 原消息展示元数据（sendername/readuins 等）
//   101 progress / 102-110 发送行为开关 / 111-115 / 201 —— 本地与搜索标记
// 需要新字段时在此追加。
constexpr uint32_t kOutboundKeepFields[] = {
    7,   // content_type —— ChatTask msgtype 的唯一来源，必留
    9,   // url
    10,  // content —— 消息体，必留
    13,  // appinfo
    18,  // referid（引用/回复语义）
    19,  // extra_content（视频号作者元数据实测在此）
    20,  // as_id
    21,  // innerkf_vid
    49,  // attach_info
};

bool is_keep_field(uint32_t field) {
    for (uint32_t f : kOutboundKeepFields) {
        if (f == field) {
            return true;
        }
    }
    return false;
}

// 读一个 varint；越界/超长返回 false
bool read_varint(const std::vector<uint8_t> &buf, size_t &pos, uint64_t &out) {
    uint64_t value = 0;
    int shift = 0;
    while (true) {
        if (pos >= buf.size() || shift > 63) {
            return false;
        }
        const uint8_t b = buf[pos++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            out = value;
            return true;
        }
        shift += 7;
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// wire 级字段区间：start/end 为整个字段（含 tag 头）在原始流中的字节范围
struct FieldSpan {
    uint32_t field;
    uint32_t wire;
    size_t start;       // 含 tag 的字段起始
    size_t end;         // 字段结束（独占）
    size_t value_start; // varint 值 / wire2 payload 的起点（长度头之后）
};

// 从头扫到尾切出全部字段区间。返回 false = 流残缺/非法（日志已指明 offset）。
bool scan_fields(const std::vector<uint8_t> &raw, std::vector<FieldSpan> &spans) {
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t start = pos;
        uint64_t tag = 0;
        if (!read_varint(raw, pos, tag)) {
            LOGE("[raw_pb] PB wire 校验失败 @offset %zu：tag varint 残缺——捕获流被截断，拒绝发送", pos);
            return false;
        }
        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7);
        const size_t value_start = pos;
        if (wire == 0) {
            uint64_t value = 0;
            if (!read_varint(raw, pos, value)) {
                LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u varint 残缺——捕获流被截断，拒绝发送",
                     pos, field);
                return false;
            }
        } else if (wire == 2) {
            uint64_t len = 0;
            if (!read_varint(raw, pos, len)) {
                LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u 长度头残缺——捕获流被截断，拒绝发送",
                     pos, field);
                return false;
            }
            if (len > raw.size() - pos) {
                LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u 长度 %llu 越界（剩余 %zu 字节）"
                     "——捕获流被截断，拒绝发送",
                     pos, field, static_cast<unsigned long long>(len), raw.size() - pos);
                return false;
            }
            pos += static_cast<size_t>(len);
        } else if (wire == 1) {
            if (raw.size() - pos < 8) {
                LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u fixed64 残缺——捕获流被截断，拒绝发送",
                     pos, field);
                return false;
            }
            pos += 8;
        } else if (wire == 5) {
            if (raw.size() - pos < 4) {
                LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u fixed32 残缺——捕获流被截断，拒绝发送",
                     pos, field);
                return false;
            }
            pos += 4;
        } else {
            LOGE("[raw_pb] PB wire 校验失败 @offset %zu：字段 %u 非法 wire type %u", start, field, wire);
            return false;
        }
        spans.push_back({field, wire, start, pos, value_start});
    }
    return true;
}

} // namespace

bool parse_hex_stream(const std::string &hex_text, std::vector<uint8_t> &out) {
    out.clear();
    out.reserve(hex_text.size() / 3);
    size_t line_no = 1;
    int hi = -1;               // 当前 token 已累积的高半字节；-1 = token 为空
    bool zero_pending = false; // 当前 token 恰好只有 '0'（用于吞 "0x" 前缀）

    for (size_t i = 0; i < hex_text.size(); ++i) {
        const char c = hex_text[i];
        const int v = hex_digit(c);
        if (v >= 0) {
            if (hi < 0) {
                hi = v;
                zero_pending = (v == 0);
            } else {
                out.push_back(static_cast<uint8_t>((hi << 4) | v));
                hi = -1;
                zero_pending = false;
            }
            continue;
        }
        if ((c == 'x' || c == 'X') && hi == 0 && zero_pending) {
            // "0x" 前缀：吞掉刚累积的 '0'
            hi = -1;
            zero_pending = false;
            continue;
        }
        // 分隔符 / 注释 / 非法字符：先统一检查半字节悬挂（报错行号为当前行）
        const bool is_sep = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';');
        const bool is_comment = (c == '#');
        if (is_sep || is_comment) {
            if (hi >= 0) {
                LOGE("[raw_pb] pb.hex 第 %zu 行：奇数个十六进制字符（半字节）——捕获流可能被截断",
                     line_no);
                out.clear();
                return false;
            }
            if (is_comment) {
                while (i + 1 < hex_text.size() && hex_text[i + 1] != '\n') {
                    ++i;
                }
            } else if (c == '\n') {
                line_no++;
            }
            continue;
        }
        LOGE("[raw_pb] pb.hex 第 %zu 行：非法字符 '%c' (0x%02x)", line_no,
             (c >= 0x20 && c < 0x7f) ? c : '?', static_cast<unsigned char>(c));
        out.clear();
        return false;
    }
    if (hi >= 0) {
        LOGE("[raw_pb] pb.hex 末尾悬挂半字节——捕获流可能被截断");
        out.clear();
        return false;
    }
    if (out.empty()) {
        LOGE("[raw_pb] pb.hex 未解析到任何字节");
        return false;
    }
    return true;
}

std::vector<uint8_t> sanitize_message_pb_for_send(const std::vector<uint8_t> &raw,
                                                  size_t *stripped_fields) {
    if (stripped_fields) {
        *stripped_fields = 0;
    }
    if (raw.empty()) {
        LOGE("[raw_pb] 输入 PB 为空");
        return {};
    }
    std::vector<FieldSpan> spans;
    if (!scan_fields(raw, spans)) {
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(raw.size());
    size_t dropped = 0;
    for (const FieldSpan &s : spans) {
        if (is_keep_field(s.field)) {
            out.insert(out.end(),
                       raw.begin() + static_cast<std::ptrdiff_t>(s.start),
                       raw.begin() + static_cast<std::ptrdiff_t>(s.end));
        } else {
            dropped++;
        }
    }
    if (stripped_fields) {
        *stripped_fields = dropped;
    }
    return out;
}

bool validate_message_pb(const std::vector<uint8_t> &raw) {
    std::vector<FieldSpan> spans;
    return scan_fields(raw, spans);
}

int extract_content_type(const std::vector<uint8_t> &pb) {
    // 尽力而为，不走 scan_fields 的全流严格校验：逐字段前进，扫到 Tag 7
    // varint 立即返回——Tag 7 之后的 wire 截断（如小程序粘贴流：content
    // 声明 2375 字节实际只剩 ~1980）不影响取值，截断流的横幅/失败日志也
    // 能报出 content_type。Tag 7 之前就残缺才取不到，返回 -1。
    size_t pos = 0;
    while (pos < pb.size()) {
        uint64_t tag = 0;
        if (!read_varint(pb, pos, tag)) {
            return -1;
        }
        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire = static_cast<uint32_t>(tag & 7);
        if (wire == 0) {
            uint64_t value = 0;
            if (!read_varint(pb, pos, value)) {
                return -1;
            }
            if (field == 7 && value <= 0x7FFFFFFFu) {
                return static_cast<int>(value);
            }
        } else if (wire == 2) {
            uint64_t len = 0;
            if (!read_varint(pb, pos, len) || len > pb.size() - pos) {
                return -1;
            }
            pos += static_cast<size_t>(len);
        } else if (wire == 1) {
            if (pb.size() - pos < 8) {
                return -1;
            }
            pos += 8;
        } else if (wire == 5) {
            if (pb.size() - pos < 4) {
                return -1;
            }
            pos += 4;
        } else {
            return -1; // 非法 wire type（3/4/6/7）
        }
    }
    return -1;
}

} // namespace rawpb
