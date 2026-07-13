//
// Created for SoHook project
// 纯手工 Protobuf 编码器 —— 零外部依赖，替代 protobuf 静态库
//
// 支持功能：varint、length-delimited、embedded message 编码
// 完全满足 generate_conversation_proto / generate_image_message_proto / generate_text_message_proto 的需求
//

#ifndef SOHOOK_MANUAL_PB_ENCODER_H
#define SOHOOK_MANUAL_PB_ENCODER_H

#include <string>
#include <vector>
#include <cstdint>

namespace pb {

// ─── Protobuf wire types ─────────────────────────────────
enum WireType : uint8_t {
    WIRE_VARINT          = 0,  // int32, int64, uint32, uint64, bool, enum
    WIRE_LENGTH_DELIMITED = 2,  // string, bytes, embedded message, packed repeated
};

// ─── 构造 tag ───────────────────────────────────────────
// tag = (field_number << 3) | wire_type
inline constexpr uint64_t make_tag(uint32_t field_number, WireType wire_type) {
    return (static_cast<uint64_t>(field_number) << 3) | static_cast<uint64_t>(wire_type);
}

// ─── varint 编码 ────────────────────────────────────────
inline void write_varint(uint64_t value, std::string& out) {
    while (value >= 0x80u) {
        out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

// ─── tag + 字段编码 ─────────────────────────────────────

// field_number + 固定 uint32 值
inline void write_uint32(uint32_t field_number, uint32_t value, std::string& out) {
    write_varint(make_tag(field_number, WIRE_VARINT), out);
    write_varint(value, out);
}

// field_number + 固定 uint64 值
inline void write_uint64(uint32_t field_number, uint64_t value, std::string& out) {
    write_varint(make_tag(field_number, WIRE_VARINT), out);
    write_varint(value, out);
}

// field_number + string/bytes
inline void write_string(uint32_t field_number, const std::string& value, std::string& out) {
    write_varint(make_tag(field_number, WIRE_LENGTH_DELIMITED), out);
    write_varint(value.size(), out);
    out.append(value);
}

// field_number + 嵌套的子 message（已序列化为 string 的原始字节）
inline void write_message(uint32_t field_number, const std::string& serialized_sub_msg, std::string& out) {
    write_varint(make_tag(field_number, WIRE_LENGTH_DELIMITED), out);
    write_varint(serialized_sub_msg.size(), out);
    out.append(serialized_sub_msg);
}

} // namespace pb

#endif // SOHOOK_MANUAL_PB_ENCODER_H
