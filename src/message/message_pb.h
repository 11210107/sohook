//
// Created by user_wangzhen on 2026/8/4.
//
#pragma once
#include <vector>

std::vector<uint8_t> generate_text_message_pb(const std::string& text_content);
std::vector<uint8_t> generate_image_message_pb(
    const std::string &file_path,
    int width,
    int height,
    uint64_t file_size,
    bool is_hd,
    const std::string &thumb_path = "",
    const std::string &mid_thumb_path = "",
    int ld_width = 0,
    int ld_height = 0
);
std::vector<uint8_t> generate_file_message_pb(
    const std::string &file_path,
    uint64_t file_size
);