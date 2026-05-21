//
// Created by user_wangzhen on 2026/5/20.
//

#ifndef SOHOOK_PROTOCOL_UTILS_H
#define SOHOOK_PROTOCOL_UTILS_H
#include <string>
#include <vector>

std::string generate_conversation_proto(uint64_t conversation_id);

std::vector<uint8_t> generate_image_message_proto(
    const std::string &file_name,
    const std::string &orig_path,
    uint64_t file_size,
    uint32_t width, uint32_t height,
    uint32_t thumb_w, uint32_t thumb_h,
    const std::string &thumb_path = "",
    const std::string &mid_path = ""
);

std::vector<uint8_t> generate_text_message_proto(const std::string &text_content);

void dump_protobuf_hex(const std::vector<uint8_t> &data);
#endif //SOHOOK_PROTOCOL_UTILS_H
