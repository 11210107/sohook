//
// Created by user_wangzhen on 2026/5/20.
//
// 🔧 使用 proto/gen/ 下的 pb 类 + protobuf 静态库
//
#include "protocol_utils.h"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include "logger.h"
#include "wework_conversation.pb.h"
#include "wework_image_message.pb.h"
#include "wework_text_message.pb.h"
// nativeIsVid 判断是否为 VID (对应底层 Native 实现: HIWORD(a3) == 6)
static inline bool is_vid(uint64_t id) {
    return (id >> 48) == 6;
}
/**
 * 使用 protobuf 生成类构造会话信息 PB 流
 * 对应 proto/wework_conversation.proto -> sohook::ConversationInfo
 */
std::string generate_conversation_proto(uint64_t conversation_id) {
    // 判断是否为联系人会话（以 "788" 开头）
    bool is_contact = is_vid(conversation_id);

    sohook::ConversationInfo info;
    info.set_conversation_id(conversation_id);
    info.set_field_12(is_contact ? 0 : 1);
    info.set_field_15("");

    std::string out;
    info.SerializeToString(&out);
    return out;
}

/**
 * 使用 protobuf 生成类构造图片消息 PB 流
 * 对应 proto/wework_image_message.proto -> sohook::ImageMessage
 */
std::vector<uint8_t> generate_image_message_proto(
    const std::string& file_name,       // 20260414104931_149_83.jpg
    const std::string& orig_path,       // 原图绝对路径
    uint64_t file_size,                 // 文件大小 (字节)
    uint32_t width, uint32_t height,    // 原图宽高
    uint32_t thumb_w, uint32_t thumb_h, // 缩略图宽高
    const std::string& thumb_path,      // 缩略图路径
    const std::string& mid_path         // 中等图路径
) {
    std::string final_thumb = thumb_path.empty() ? orig_path : thumb_path;
    std::string final_mid   = mid_path.empty()   ? orig_path : mid_path;

    sohook::ImageMessage msg;
    msg.set_msg_type(7);

    sohook::ImageContent* content = msg.mutable_content();
    content->set_file_name(file_name);
    content->set_orig_path(orig_path);
    content->set_file_size(file_size);
    content->set_width(width);
    content->set_height(height);
    content->set_thumb_width(thumb_w);
    content->set_thumb_height(thumb_h);
    content->set_thumb_path(final_thumb);
    content->set_mid_path(final_mid);

    std::string out;
    msg.SerializeToString(&out);
    return std::vector<uint8_t>(out.begin(), out.end());
}

/**
 * 使用 protobuf 生成类构造文本消息 PB 流
 * 对应 proto/wework_text_message.proto -> sohook::TextMessage
 */
std::vector<uint8_t> generate_text_message_proto(const std::string& text_content) {
    sohook::TextMessage msg;

    sohook::TextMessageCore* core = msg.mutable_core();
    sohook::TextContentWrapper* wrapper = core->mutable_wrapper();
    wrapper->set_status(0);

    sohook::TextContent* text = wrapper->mutable_content();
    text->set_text(text_content);

    std::string out;
    msg.SerializeToString(&out);
    return std::vector<uint8_t>(out.begin(), out.end());
}

/**
 * 绚丽的 Hex Dump 打印工具：完美对齐 Mac 的 xxd / Hex Fiend 格式
 */
void dump_protobuf_hex(const std::vector<uint8_t>& data) {
    LOGI("========= [Protobuf Hex Dump 开始] 大小: %zu 字节 =========", data.size());

    std::stringstream hex_stream;
    std::stringstream ascii_stream;

    for (size_t i = 0; i < data.size(); ++i) {
        // 1. 拼接十六进制字节
        hex_stream << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";

        // 2. 拼接 ASCII 预览（不可见字符用点 '.' 代替）
        if (data[i] >= 32 && data[i] <= 126) {
            ascii_stream << (char)data[i];
        } else {
            ascii_stream << ".";
        }

        // 3. 每 16 个字节换一行，或者到最后一个字节时输出
        if ((i + 1) % 16 == 0 || (i + 1) == data.size()) {
            // 如果最后一行不满 16 字节，用空格补齐 Hex 区域以对齐 ASCII 预览
            if ((i + 1) % 16 != 0) {
                size_t missing = 16 - ((i + 1) % 16);
                for (size_t m = 0; m < missing; ++m) hex_stream << "   ";
            }

            // 打印出当前行：[行号] 十六进制字节数据  |  ASCII 预览
            LOGI("[%04zX] %s |  %s", (i / 16) * 16, hex_stream.str().c_str(), ascii_stream.str().c_str());

            // 清空缓存准备下一行
            hex_stream.str(""); hex_stream.clear();
            ascii_stream.str(""); ascii_stream.clear();
        }
    }
    LOGI("========= [Protobuf Hex Dump 结束] =========");
}
