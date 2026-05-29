//
// Created by user_wangzhen on 2026/5/20.
//
#include "protocol_utils.h"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include "logger.h"

// 辅助函数：将 uint64_t 编码为 Protobuf 的 Base128 Varint 格式
void encode_varint(uint64_t value, std::vector<uint8_t>& output) {
    while (value >= 0x80) {
        output.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<uint8_t>(value & 0x7F));
}

// 🧪 实验方案：完全剔除字段 1，只保留 ConversationID 和后续字段
std::string generate_conversation_proto(uint64_t conversation_id) {
    std::vector<uint8_t> proto_bytes;
    bool is_contact = std::to_string(conversation_id).rfind("788", 0) == 0;
    // -----------------------------------------------------------------
    // 1. 写入字段 1 (图中 0-10 字节区)
    // Tag = (1 << 3) | 0 (Varint) = 0x08
    // Value = 7593293522086153122
    // -----------------------------------------------------------------
    // proto_bytes.push_back(0x08);
    // encode_varint(7593293522086153122ULL, proto_bytes);

    // 1. 直接写入字段 2 (ConversationID)
    // Tag = (2 << 3) | 0 (Varint) = 0x10
    proto_bytes.push_back(0x10);
    encode_varint(conversation_id, proto_bytes);

    // 2. 写入字段 12
    // Tag = (12 << 3) | 0 (Varint) = 0x60
    proto_bytes.push_back(0x60);
    if (!is_contact) {
        // 群聊图中 Byte 19-21 为 60 01，解码值为 1
        proto_bytes.push_back(0x01);
    } else {
        // 单聊原方案中该值为 0
        proto_bytes.push_back(0x00);
    }

    // 3. 写入字段 15
    // Tag = (15 << 3) | 2 (Length-delimited) = 0x7A
    proto_bytes.push_back(0x7A);
    proto_bytes.push_back(0x00);

    return std::string(reinterpret_cast<char*>(proto_bytes.data()), proto_bytes.size());
}



// 字符串/子消息 (Length-delimited) 编码器
static void encode_length_delimited(uint32_t field_number, const std::vector<uint8_t>& data, std::vector<uint8_t>& output) {
    uint32_t tag = (field_number << 3) | 2;
    encode_varint(tag, output);
    encode_varint(data.size(), output);
    output.insert(output.end(), data.begin(), data.end());
}

static void encode_string_field(uint32_t field_number, const std::string& str, std::vector<uint8_t>& output) {
    uint32_t tag = (field_number << 3) | 2;
    encode_varint(tag, output);
    encode_varint(str.size(), output);
    output.insert(output.end(), str.begin(), str.end());
}

static void encode_varint_field(uint32_t field_number, uint64_t value, std::vector<uint8_t>& output) {
    uint32_t tag = (field_number << 3) | 0;
    encode_varint(tag, output);
    encode_varint(value, output);
}
/**
 * 💡 动态生成企业微信图片消息 PB 流
 * 💡 修正点：返回值改为 std::vector<uint8_t>
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
    std::vector<uint8_t> root_bytes;

    // 1. 写入顶层字段 7 (Varint) -> 固定的 7
    encode_varint_field(7, 7, root_bytes);

    // 2. 开始构建嵌套在里面的子数据流
    std::vector<uint8_t> sub_msg_bytes;

    encode_string_field(2, file_name, sub_msg_bytes);     // 子字段 2: 文件名
    encode_string_field(3, orig_path, sub_msg_bytes);     // 子字段 3: 原图路径
    encode_varint_field(4, file_size, sub_msg_bytes);     // 子字段 4: 文件大小
    encode_varint_field(5, width, sub_msg_bytes);         // 子字段 5: 宽度
    encode_varint_field(6, height, sub_msg_bytes);        // 子字段 6: 高度
    encode_varint_field(28, thumb_w, sub_msg_bytes);      // 子字段 28: 缩略图宽
    encode_varint_field(29, thumb_h, sub_msg_bytes);      // 子字段 29: 缩略图高

    std::string final_thumb = thumb_path.empty() ? orig_path : thumb_path;
    std::string final_mid = mid_path.empty() ? orig_path : mid_path;

    encode_string_field(202, final_thumb, sub_msg_bytes); // 子字段 202: 缩略图路径
    encode_string_field(203, final_mid, sub_msg_bytes);   // 子字段 203: 中等图路径

    // -----------------------------------------------------------------
    // 3. 💡 核心修正点：
    // 根据原始图的解析结果，外壳包裹容器的 Field Number 必须是 10，而不是 2！
    // -----------------------------------------------------------------
    encode_length_delimited(10, sub_msg_bytes, root_bytes);

    // 4. 打包返回
    return root_bytes;
}

// 💡 绚丽的 Hex Dump 打印工具：完美对齐 Mac 的 xxd / Hex Fiend 格式
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

/**
 * 💡 动态生成企业微信文本消息 PB 流
 * @param text_content 想要发送的纯文本内容（如 "很大的"）
 * @return 完美对齐原始外壳的二进制字节流 (对应 Field 10 外壳)
 */
std::vector<uint8_t> generate_text_message_proto(const std::string& text_content) {
    std::vector<uint8_t> root_bytes;

    // -------------------------------------------------------------
    // 【第四层】：构建最内层的文本包裹（Field 1 -> 纯文本内容）
    // 对应 Hex 尾部的：0a 09 [e5 be 88...]
    // -------------------------------------------------------------
    std::vector<uint8_t> layer4_bytes;
    // 子字段 1 (String): 真正的文本字符串
    encode_string_field(1, text_content, layer4_bytes);

    // -------------------------------------------------------------
    // 【第三层】：构建文本内容的二级容器（包含状态码和刚才的文本包裹）
    // 对应 Hex 中的：08 00 12 0b...
    // -------------------------------------------------------------
    std::vector<uint8_t> layer3_bytes;
    // 子字段 1 (Varint): 状态/类型标记位，固定写 0
    encode_varint_field(1, 0, layer3_bytes);
    // 子字段 2 (Length-delimited): 将第四层的文本包裹装载进来
    encode_length_delimited(2, layer4_bytes, layer3_bytes);

    // -------------------------------------------------------------
    // 【第二层】：构建文本消息的核心包裹外壳
    // 对应 Hex 中的：0a 0f...
    // -------------------------------------------------------------
    std::vector<uint8_t> layer2_bytes;
    // 子字段 1 (Length-delimited): 将第三层的混合数据整体装载进来
    encode_length_delimited(1, layer3_bytes, layer2_bytes);

    // -------------------------------------------------------------
    // 【第一层 / 顶层】：注入企微统一的消息路由大外壳 Field 10
    // 对应 Hex 开头的：52 11...
    // -------------------------------------------------------------
    encode_length_delimited(10, layer2_bytes, root_bytes);

    // 完美打包返回
    return root_bytes;
}